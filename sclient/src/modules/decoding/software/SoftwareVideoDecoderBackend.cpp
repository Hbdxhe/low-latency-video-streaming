#include "modules/decoding/VideoDecoderBackend.h"

// FFmpeg 是 C 语言库。
// C++ 代码 include C 头文件时，通常要放在 extern "C" 里，
// 避免 C++ 名字改编导致链接失败。
extern "C" {
#include <libavcodec/avcodec.h> // 引入 FFmpeg 编解码库
#include <libavutil/error.h>    // 引入 FFmpeg 错误处理工具
#include <libavutil/pixfmt.h>   // 引入 FFmpeg 像素格式定义
}

#include <string>

namespace sclient {

namespace {

// 把 FFmpeg 的 AVPixelFormat 转成项目自己的 DecodedPixelFormat。
//
// 为什么要转换？
// 因为渲染模块不应该直接依赖 FFmpeg 的枚举。
// 解码模块输出项目统一格式，后面的 OpenGL 渲染模块只认识项目自己的类型。
DecodedPixelFormat ResolveDecodedPixelFormat(AVPixelFormat pixel_format) {

    switch (pixel_format) {
        // FFmpeg 标准 YUV420P
        case AV_PIX_FMT_YUV420P:

        // YUVJ420P 是早期 FFmpeg 里用于表示 full range 的 YUV420P
        case AV_PIX_FMT_YUVJ420P:
            return DecodedPixelFormat::kYuv420p;

        // NV12 是另一种常见 YUV420 格式
        case AV_PIX_FMT_NV12:
            return DecodedPixelFormat::kNv12;
        default:
            return DecodedPixelFormat::kUnknown;
    }
}

// 把 FFmpeg 错误码转换成人能看懂的字符串
std::string AvErrorString(int error_code) {
    // FFmpeg 要求的错误字符串缓冲区
    char buffer[AV_ERROR_MAX_STRING_SIZE] = {0};

    // 把 error_code 转成字符串，写入 buffer
    av_strerror(error_code, buffer, sizeof(buffer));
    return std::string(buffer);
}

// 把 FFmpeg 解出来的 AVFrame 信息“映射”到项目自己的 DecodedFrame。
//
// 注意：这里不是复制 YUV 像素数据。
// 它只是把 data 指针、linesize、宽高、像素格式填到 DecodedFrame。
// 真正的内存仍然由 AVFrame 持有。
void CopyDecodedFrameView(const AVFrame &source, DecodedFrame *decoded_frame) {
    if (decoded_frame == nullptr) {
        return;
    }

    decoded_frame->width = source.width;
    decoded_frame->height = source.height;

    // 把 FFmpeg 像素格式转成项目自己的像素格式
    decoded_frame->pixel_format = ResolveDecodedPixelFormat(static_cast<AVPixelFormat>(source.format));

    // decoded_frame->data 通常是一个数组，
    // 保存每个图像平面的起始地址。
    //
    // YUV420P 通常：
    // data[0] = Y 平面
    // data[1] = U 平面
    // data[2] = V 平面
    //
    // NV12 通常：
    // data[0] = Y 平面
    // data[1] = UV 交错平面
    for (std::size_t index = 0; index < decoded_frame->data.size(); ++index) {
        // 保存第 index 个平面的数据指针
        decoded_frame->data[index] = source.data[index];

        // 保存第 index 个平面每一行占多少字节
        decoded_frame->linesize[index] = source.linesize[index];
    }
}

// 为 AVFrame 创建一个 shared_ptr owner。
//
// 这个函数解决一个非常重要的问题：
// DecodedFrame 里的 data 指针指向 AVFrame 内部 buffer。如果 AVFrame 被释放，data 指针就悬空了。
// 所以这里创建 shared_ptr<void> 持有 AVFrame，等 DecodedFrame 不再使用时，自动释放 AVFrame。
std::shared_ptr<void> MakeAvFrameOwner(AVFrame *frame) {
    // 创建 shared_ptr<void>。
    //
    // 第一个参数是要管理的指针。
    // 第二个参数是自定义删除器。
    return std::shared_ptr<void>(frame, [](void *pointer) {
        AVFrame *owned_frame = reinterpret_cast<AVFrame *>(pointer);
        // 释放 AVFrame。
        //
        // av_frame_free 会释放 AVFrame 结构本身，并减少内部引用计数，释放它引用的图像数据
        av_frame_free(&owned_frame);
    });
}

class SoftwareVideoDecoderBackend final : public VideoDecoderBackend {
public:
    ~SoftwareVideoDecoderBackend() override {
        Shutdown();
    }

    // 初始化 FFmpeg H.264 解码器
    bool Initialize(std::string *error_message) override {
        Shutdown();

        // 查找 H.264 解码器。
        //
        // AV_CODEC_ID_H264 表示要找 H.264 decoder。
        // FFmpeg 会返回一个 AVCodec 描述对象。
        codec_ = avcodec_find_decoder(AV_CODEC_ID_H264);
        if (codec_ == nullptr) {
            if (error_message != nullptr) {
                *error_message = "failed to find H.264 decoder";
            }
            return false;
        }

        // 根据 codec_ 分配解码器上下文。
        //
        // AVCodecContext 是 FFmpeg 解码器运行时状态，
        // 里面保存解码参数、内部缓冲、线程状态等。
        codec_context_ = avcodec_alloc_context3(codec_);
        if (codec_context_ == nullptr) {
            if (error_message != nullptr) {
                *error_message = "failed to allocate decoder context";
            }
            return false;
        }

        // 打开 H.264 解码器。
        //
        // 第三个参数 nullptr 表示不传额外配置选项
        if (avcodec_open2(codec_context_, codec_, nullptr) < 0) {
            if (error_message != nullptr) {
                *error_message = "failed to open H.264 decoder";
            }
            Shutdown();
            return false;
        }

        // 分配一个 AVFrame，用于接收 FFmpeg 解码输出
        decoded_frame_ = av_frame_alloc();
        if (decoded_frame_ == nullptr) {
            if (error_message != nullptr) {
                *error_message = "failed to allocate decode frame";
            }
            Shutdown();
            return false;
        }

        return true;
    }

    // 解码一段 H.264 编码数据。
    //
    // data：H.264 字节数据，通常是 Annex-B 格式。
    // size：data 长度。
    // decoded_frame：输出的解码后图像。
    // error_message：错误信息。
    bool Decode(
            const std::uint8_t *data,
            std::size_t size,
            DecodedFrame *decoded_frame,
            std::string *error_message) override {

        if (codec_context_ == nullptr ||
            decoded_frame_ == nullptr ||
            decoded_frame == nullptr ||
            data == nullptr ||
            size == 0) {
            if (error_message != nullptr) {
                *error_message = "decoder is not initialized";
            }
            return false;
        }

        // 重置输出 DecodedFrame。
        //
        // 避免上一次解码结果残留。
        *decoded_frame = DecodedFrame();

        // 清理内部 AVFrame 上一次持有的数据引用。
        //
        // 注意不是释放 decoded_frame_ 本身，而是把它恢复成可复用的空 frame。
        av_frame_unref(decoded_frame_);

        // 创建一个 AVPacket
        AVPacket packet{};
        packet.data = const_cast<std::uint8_t *>(data);
        packet.size = static_cast<int>(size);

        // 把压缩数据送入 FFmpeg 解码器
        const int send_result = avcodec_send_packet(codec_context_, &packet);
        if (send_result < 0) {
            if (error_message != nullptr) {
                *error_message = "avcodec_send_packet failed: " + AvErrorString(send_result);
            }
            return false;
        }

        // 标记本次 Decode 是否真的产出了解码帧。
        bool produced_frame = false;

        // 用于保存最终要交给 DecodedFrame 持有的 AVFrame。
        //
        // 为什么不用 decoded_frame_ 直接给出去？
        // 因为 decoded_frame_ 是内部复用对象，
        // 下一次 Decode 时会 av_frame_unref(decoded_frame_)。
        // 所以需要 clone 一份，交给输出 DecodedFrame 持有。
        AVFrame *owned_frame = nullptr;

        // FFmpeg 可能一次 send_packet 后，输出 0 帧、1 帧或多帧。
        // 所以这里循环 receive。
        while (true) {
            // 从 FFmpeg 解码器取一帧解码后的图像
            const int receive_result = avcodec_receive_frame(codec_context_, decoded_frame_);

            // AVERROR(EAGAIN) 表示：当前没有更多 frame 可以取了，需要继续 send 新 packet。
            // AVERROR_EOF 表示： 解码器已经结束。
            if (receive_result == AVERROR(EAGAIN) || receive_result == AVERROR_EOF) {
                break;
            }

            if (receive_result < 0) {
                if (error_message != nullptr) {
                    *error_message = "avcodec_receive_frame failed: " + AvErrorString(receive_result);
                }
                return false;
            }

            // 如果解码出来的 frame 宽高不合法
            if (decoded_frame_->width <= 0 || decoded_frame_->height <= 0) {
                av_frame_unref(decoded_frame_);
                continue;
            }

            // 克隆当前解码出来的 AVFrame。
            //
            // av_frame_clone 不是简单浅拷贝。
            // 它会创建新的 AVFrame，并引用同一份底层 buffer。
            // 这样 cloned_frame 可以独立管理生命周期。
            AVFrame *cloned_frame = av_frame_clone(decoded_frame_);

            // 清理内部复用 frame，准备下次 receive
            av_frame_unref(decoded_frame_);

            // 如果 clone 失败
            if (cloned_frame == nullptr) {
                if (owned_frame != nullptr) {
                    av_frame_free(&owned_frame);
                }
                if (error_message != nullptr) {
                    *error_message = "failed to clone decoded frame";
                }
                return false;
            }

            // 如果之前已经产出过一帧 owned_frame，
            // 这里释放旧的，只保留最新的一帧。
            //
            // 实时视频里通常更关注最新帧。
            if (owned_frame != nullptr) {
                av_frame_free(&owned_frame);
            }

            // 保存当前 clone 出来的 frame。
            owned_frame = cloned_frame;
            produced_frame = true;
        }

         // 如果本次 Decode 最终产出了解码帧
        if (produced_frame) {

            // 让输出 DecodedFrame 持有 cloned AVFrame 的生命周期。
            //
            // 这样 decoded_frame->data 指针在渲染期间不会失效。
            decoded_frame->owner = MakeAvFrameOwner(owned_frame);

            // 把 AVFrame 的宽高、像素格式、data 指针、linesize
            // 拷贝到项目自己的 DecodedFrame 视图里。
            CopyDecodedFrameView(*owned_frame, decoded_frame);
        }

        // 如果没有产出 frame，并且调用者需要错误信息。
        if (!produced_frame && error_message != nullptr) {
            *error_message = "decoder did not output a frame";
        }
        return produced_frame;
    }

    // 关闭解码器，释放 FFmpeg 资源。
    void Shutdown() override {
        // 如果内部 AVFrame 存在
        if (decoded_frame_ != nullptr) {
            av_frame_free(&decoded_frame_);
        }

        // 如果解码器上下文存在
        if (codec_context_ != nullptr) {
            avcodec_free_context(&codec_context_);
        }

        codec_ = nullptr;
    }

    const std::string &backend_name() const override {
        return backend_name_;
    }

private:
    // FFmpeg H.264 解码器描述对象
    const AVCodec *codec_ = nullptr;

    // FFmpeg 解码器上下文
    AVCodecContext *codec_context_ = nullptr;

    // 内部复用的 AVFrame
    AVFrame *decoded_frame_ = nullptr;
    const std::string backend_name_ = "software";
};

}  // namespace

// 创建软件视频解码后端的工厂函数
std::unique_ptr<VideoDecoderBackend> CreateSoftwareVideoDecoderBackend() {
    return std::unique_ptr<VideoDecoderBackend>(new SoftwareVideoDecoderBackend());
}

}  // namespace sclient
