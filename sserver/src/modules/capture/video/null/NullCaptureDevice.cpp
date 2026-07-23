#include "modules/capture/video/null/NullCaptureDevice.h"

#include <chrono>
#include <cstring>
#include <sstream>
#include <thread>

#include "common/time/MonotonicClock.h"

namespace sserver {
namespace modules {
namespace capture {

NullCaptureDevice::NullCaptureDevice(
        const config::CaptureConfig &config,
        const config::CodecConfig &codec_config)
        // NullCaptureDevice 不依赖真实硬件，配置决定它生成文本帧还是 H.264 测试图案。
        : config_(config),
          codec_config_(codec_config),
          opened_(false),
          streaming_(false),
          sequence_(0) {
}

bool NullCaptureDevice::Open() {
    // NullCaptureDevice 是一个“虚拟采集源”，不依赖真实摄像头设备。
    //
    // 与 V4L2CaptureDevice 不同，它不需要：
    // 1. stat() 检查 /dev/video0；
    // 2. open() 打开设备文件；
    // 3. ioctl() 配置摄像头；
    // 4. mmap() 映射采集 buffer。
    //
    // 因此这里仅记录 opened_ 状态，表示该虚拟设备已经可用。
    opened_ = true;
    return true;
}

bool NullCaptureDevice::Start() {
    if (!opened_) {
        return false;
    }

    if (config_.null_payload_mode == "h264_test_pattern") {
        // h264_test_pattern 模式会先合成一帧 YUYV422 原始图像，
        // 再复用 VideoEncoder/x264 编码成真正可解码的 H.264。
        // 这让我们在没有摄像头时，也能跑通“编码 -> 传输 -> 解码 -> 渲染”完整链路。
        encoder_ = std::make_unique<modules::encoding::VideoEncoder>();
        std::string error_message;
        if (!encoder_->Initialize(
                    config_.width,
                    config_.height,
                    config_.fps,
                    codec_config_,
                    &error_message)) {
            encoder_.reset();
            return false;
        }

        // 分配一帧合成 YUYV422 图像的存储空间。
        //
        // YUYV422 是 packed 格式，每 2 个像素占 4 字节，
        // 因此平均每个像素占 2 字节。
        //
        // 所以一帧图像大小为：
        //      width * height * 2
        synthetic_yuyv_frame_.assign(static_cast<std::size_t>(config_.width * config_.height * 2), 0);
    }

    streaming_ = true;
    return true;
}

common::model::EncodedFramePtr NullCaptureDevice::CaptureFrame() {
    // 如果虚拟采集源没有启动，则不能产出帧
    if (!streaming_) {
        return common::model::EncodedFramePtr();
    }

    // 用 sleep 模拟采集帧间隔，避免测试源无限速地产帧。
    std::this_thread::sleep_for(std::chrono::milliseconds(config_.frame_interval_ms));
    const std::uint64_t capture_timestamp_ns = common::time::MonotonicNowNs();

    // 构造统一的 EncodedFrame。下游 TransportModule 不关心它来自真实摄像头还是 Null 源。
    auto frame = std::make_shared<common::model::EncodedFrame>();
    frame->sequence = sequence_++;
    frame->type = common::model::StreamPayloadType::kVideo;
    frame->capture_timestamp_ns = capture_timestamp_ns;

    if (config_.null_payload_mode == "h264_test_pattern" && encoder_ != nullptr) {
        // 生成一帧随 sequence 变化的 YUYV 测试图像。
        FillSyntheticYuyvFrame();
        frame->encode_start_timestamp_ns = common::time::MonotonicNowNs();

        // 编码成 H.264，客户端可以用 FFmpeg 解码并渲染。
        if (!encoder_->EncodeYuyv422Frame(
                    synthetic_yuyv_frame_.data(),
                    synthetic_yuyv_frame_.size(),
                    &frame->payload,
                    &frame->is_keyframe)) {
            return common::model::EncodedFramePtr();
        }
        frame->encode_end_timestamp_ns = common::time::MonotonicNowNs();

    } else {
        // text 模式：
        // 只生成普通文本 payload，不生成真实 H.264 码流。
        //
        // 该模式适合测试：
        // 1. 采集模块是否能持续产帧；
        // 2. TransportModule 是否能发送 payload；
        // 3. 接收端是否能收到数据；
        // 4. 序号和时间戳是否正常。
        //
        // 但是 text payload 不是合法视频码流，
        // 因此客户端不能把它当作 H.264 解码渲染。
        frame->encode_start_timestamp_ns = capture_timestamp_ns;
        frame->is_keyframe = true;

        // 生成一个简单的文本 payload，例如：
        //      null-frame-0
        //      null-frame-1
        //      null-frame-2
        std::ostringstream stream;
        stream << "null-frame-" << frame->sequence;
        const std::string text = stream.str();

         // 将字符串内容写入 frame->payload
        frame->payload.assign(text.begin(), text.end());

        // 如果配置了固定 payload 大小，则对 payload 进行填充。
        //
        // 例如 config_.null_payload_bytes = 1024，
        // 当前文本只有十几个字节，则后面补充字符，直到达到 1024 字节。
        //
        // 填充值使用 'A' + sequence % 26，
        // 可以让不同帧的填充内容略有变化，便于调试。
        if (config_.null_payload_bytes > frame->payload.size()) {
            frame->payload.resize(config_.null_payload_bytes, static_cast<std::uint8_t>('A' + (frame->sequence % 26)));
        }
        frame->encode_end_timestamp_ns = common::time::MonotonicNowNs();
    }

    return frame;
}

void NullCaptureDevice::Stop() {
    // 停止测试源，并释放可选的 x264 编码器。
    streaming_ = false;
    if (encoder_ != nullptr) {
        encoder_->Shutdown();
        encoder_.reset();
    }
}

void NullCaptureDevice::Close() {
    opened_ = false;
}

std::string NullCaptureDevice::Describe() const {
    return "null-capture-device";
}

void NullCaptureDevice::FillSyntheticYuyvFrame() {
    // 如果合成帧缓冲区为空，说明 Start() 阶段没有为测试图像分配内存。
    if (synthetic_yuyv_frame_.empty()) {
        return;
    }

    // 填充一帧随时间变化的 YUYV422 测试图案。
    //
    // YUYV422 是 packed 格式，每 2 个像素占 4 字节：
    //
    //      Y0 U Y1 V
    //
    // 含义是：
    //      Y0：第一个像素的亮度
    //      Y1：第二个像素的亮度
    //      U ：两个像素共享的 U 色度分量
    //      V ：两个像素共享的 V 色度分量
    //
    // 因为两个相邻像素共享一组 U/V，
    // 所以内层循环每次处理两个像素，即 column += 2。
    const int width = config_.width;
    const int height = config_.height;

    // phase 用于让每一帧图案产生变化。
    //
    // sequence_ 是当前帧序号。
    // 每生成一帧，sequence_ 会递增。
    //
    // 这里用 sequence_ * 7 生成一个随帧序号变化的偏移量，
    // 再对 256 取模，保证结果落在 uint8_t 的 0~255 范围内。
    //
    // 如果没有 phase，每一帧生成的测试图像都一样；
    // 加入 phase 后，客户端渲染时可以看到画面在动态变化。
    const std::uint8_t phase = static_cast<std::uint8_t>((sequence_ * 7) % 256);

    // 按行遍历整幅图像
    for (int row = 0; row < height; ++row) {
        // 每次处理两个相邻像素。
        //
        // 注意：YUYV422 要求每两个像素共享一组 U/V，
        // 因此 width 最好是偶数。
        for (int column = 0; column < width; column += 2) {
            // 计算当前两个像素在 synthetic_yuyv_frame_ 中的字节偏移。
            const std::size_t offset = static_cast<std::size_t>(row * width + column) * 2;

            // 生成第一个像素的亮度 Y0
            const std::uint8_t y0 = static_cast<std::uint8_t>((row + column + phase) % 256);

            // 生成第二个像素的亮度 Y1。
            //
            // 相比 y0 额外加 32，让相邻两个像素亮度略有差异，这样图像中会形成更明显的纹理变化。
            const std::uint8_t y1 = static_cast<std::uint8_t>((row + column + 32 + phase) % 256);

            // 生成 U 色度分量，这里让 U 随 row 和 phase 变化，
            // 因此图像在垂直方向和时间方向上会有颜色变化。
            const std::uint8_t u = static_cast<std::uint8_t>((128 + row / 2 + phase) % 256);

            // 生成 V 色度分量，这里让 V 随 column 和 phase 变化，
            // 因此图像在水平方向和时间方向上会有颜色变化。
            const std::uint8_t v = static_cast<std::uint8_t>((64 + column / 2 + phase) % 256);

            synthetic_yuyv_frame_[offset + 0] = y0;
            synthetic_yuyv_frame_[offset + 1] = u;
            synthetic_yuyv_frame_[offset + 2] = y1;
            synthetic_yuyv_frame_[offset + 3] = v;
        }
    }
}

}  // namespace capture
}  // namespace modules
}  // namespace sserver
