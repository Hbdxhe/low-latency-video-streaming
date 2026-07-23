#include "tests/latency/DecodeDisplayProbe.h"

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavutil/imgutils.h>
#include <libavutil/opt.h>
#include <libswscale/swscale.h>
}

#ifdef SSERVER_HAS_OPENCV
#include <opencv2/core.hpp>
#include <opencv2/highgui.hpp>
#endif

#include <memory>

#include "common/time/MonotonicClock.h"

namespace sserver {
namespace tests {
namespace latency {

struct DecodeDisplayProbe::Impl {
    const AVCodec *codec = nullptr;
    AVCodecContext *codec_context = nullptr;
    AVFrame *decoded_frame = nullptr;
    SwsContext *sws_context = nullptr;
#ifdef SSERVER_HAS_OPENCV
    cv::Mat bgr_frame;
#endif
    bool show_window = false;
};

DecodeDisplayProbe::DecodeDisplayProbe()
        : impl_(nullptr) {
}

DecodeDisplayProbe::~DecodeDisplayProbe() {
    Shutdown();
}

bool DecodeDisplayProbe::Initialize(bool show_window, std::string *error_message) {
    Shutdown();

    std::unique_ptr<Impl> impl(new Impl());
    impl->show_window = show_window;
    impl->codec = avcodec_find_decoder(AV_CODEC_ID_H264);
    if (impl->codec == nullptr) {
        if (error_message != nullptr) {
            *error_message = "failed to find H264 decoder";
        }
        return false;
    }

    impl->codec_context = avcodec_alloc_context3(impl->codec);
    if (impl->codec_context == nullptr) {
        if (error_message != nullptr) {
            *error_message = "failed to allocate decoder context";
        }
        return false;
    }

    if (avcodec_open2(impl->codec_context, impl->codec, nullptr) < 0) {
        if (error_message != nullptr) {
            *error_message = "failed to open H264 decoder";
        }
        Shutdown();
        return false;
    }

    impl->decoded_frame = av_frame_alloc();
    if (impl->decoded_frame == nullptr) {
        if (error_message != nullptr) {
            *error_message = "failed to allocate decoded frame";
        }
        Shutdown();
        return false;
    }

    impl_ = impl.release();
    return true;
}

bool DecodeDisplayProbe::DecodeAndPresent(
        const std::uint8_t *data,
        std::size_t size,
        std::uint64_t capture_timestamp_ns,
        std::string *error_message) {
    if (impl_ == nullptr || data == nullptr || size == 0) {
        if (error_message != nullptr) {
            *error_message = "decode probe is not initialized";
        }
        return false;
    }

    AVPacket packet{};
    packet.data = const_cast<std::uint8_t *>(data);
    packet.size = static_cast<int>(size);

    if (avcodec_send_packet(impl_->codec_context, &packet) < 0) {
        if (error_message != nullptr) {
            *error_message = "avcodec_send_packet failed";
        }
        return false;
    }

    bool produced_frame = false;
    while (true) {
        const int receive_result = avcodec_receive_frame(impl_->codec_context, impl_->decoded_frame);
        if (receive_result == AVERROR(EAGAIN) || receive_result == AVERROR_EOF) {
            break;
        }
        if (receive_result < 0) {
            if (error_message != nullptr) {
                *error_message = "avcodec_receive_frame failed";
            }
            return false;
        }

        produced_frame = true;
        const std::uint64_t decode_timestamp_ns = common::time::MonotonicNowNs();
        decode_latency_recorder_.RecordNs(decode_timestamp_ns - capture_timestamp_ns);

#ifdef SSERVER_HAS_OPENCV
        if (impl_->sws_context == nullptr ||
            impl_->bgr_frame.cols != impl_->decoded_frame->width ||
            impl_->bgr_frame.rows != impl_->decoded_frame->height) {
            if (impl_->sws_context != nullptr) {
                sws_freeContext(impl_->sws_context);
                impl_->sws_context = nullptr;
            }
            impl_->sws_context = sws_getContext(
                    impl_->decoded_frame->width,
                    impl_->decoded_frame->height,
                    static_cast<AVPixelFormat>(impl_->decoded_frame->format),
                    impl_->decoded_frame->width,
                    impl_->decoded_frame->height,
                    AV_PIX_FMT_BGR24,
                    SWS_FAST_BILINEAR,
                    nullptr,
                    nullptr,
                    nullptr);
            impl_->bgr_frame = cv::Mat(impl_->decoded_frame->height, impl_->decoded_frame->width, CV_8UC3);
        }

        std::uint8_t *destination_slices[4] = {
            impl_->bgr_frame.data, nullptr, nullptr, nullptr};
        int destination_strides[4] = {
            static_cast<int>(impl_->bgr_frame.step), 0, 0, 0};
        sws_scale(
                impl_->sws_context,
                impl_->decoded_frame->data,
                impl_->decoded_frame->linesize,
                0,
                impl_->decoded_frame->height,
                destination_slices,
                destination_strides);

        if (impl_->show_window) {
            cv::imshow("sserver-latency-benchmark", impl_->bgr_frame);
            cv::waitKey(1);
        }
#endif

        present_latency_recorder_.RecordNs(common::time::MonotonicNowNs() - capture_timestamp_ns);
        av_frame_unref(impl_->decoded_frame);
    }

    if (!produced_frame && error_message != nullptr) {
        *error_message = "decoder did not output a frame for the packet";
    }
    return produced_frame;
}

void DecodeDisplayProbe::Shutdown() {
    if (impl_ == nullptr) {
        return;
    }

#ifdef SSERVER_HAS_OPENCV
    if (impl_->show_window) {
        cv::destroyWindow("sserver-latency-benchmark");
    }
#endif
    if (impl_->sws_context != nullptr) {
        sws_freeContext(impl_->sws_context);
    }
    if (impl_->decoded_frame != nullptr) {
        av_frame_free(&impl_->decoded_frame);
    }
    if (impl_->codec_context != nullptr) {
        avcodec_free_context(&impl_->codec_context);
    }

    delete impl_;
    impl_ = nullptr;
}

const common::metrics::LatencyRecorder &DecodeDisplayProbe::decode_latency_recorder() const {
    return decode_latency_recorder_;
}

const common::metrics::LatencyRecorder &DecodeDisplayProbe::present_latency_recorder() const {
    return present_latency_recorder_;
}

}  // namespace latency
}  // namespace tests
}  // namespace sserver
