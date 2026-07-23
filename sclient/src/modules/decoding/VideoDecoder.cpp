#include "VideoDecoder.h"

#include <utility>

#include "modules/decoding/VideoDecoderBackend.h"

namespace sclient {

// 声明一个工厂函数：创建软件解码后端
std::unique_ptr<VideoDecoderBackend> CreateSoftwareVideoDecoderBackend();

namespace {

// 根据用户请求的解码后端类型，创建真正的解码 backend
std::unique_ptr<VideoDecoderBackend> CreateVideoDecoderBackend(
        DecodeBackend requested_backend,
        std::string *error_message) {

    switch (requested_backend) {
        case DecodeBackend::kAuto:
        case DecodeBackend::kSoftware:
            return CreateSoftwareVideoDecoderBackend();
        default:
            if (error_message != nullptr) {
                *error_message = "requested decode backend is not supported";
            }
            return nullptr;
    }
}

}  // namespace

struct VideoDecoderImpl {
    // 用户请求的解码后端
    DecodeBackend requested_backend = DecodeBackend::kAuto;

    // 当前实际使用的解码后端
    DecodeBackend active_backend = DecodeBackend::kSoftware;

    // 当前实际使用后端的名字
    std::string active_backend_name = "software";

    // 真正的解码后端对象
    std::unique_ptr<VideoDecoderBackend> backend;
};

VideoDecoder::VideoDecoder()
        : impl_(std::make_unique<VideoDecoderImpl>()) {
}

VideoDecoder::~VideoDecoder() {
    Shutdown();
}

// 初始化解码器。
// 这个重载没有指定 backend，所以默认使用 kAuto。
bool VideoDecoder::Initialize(
        std::string *error_message) {
    return Initialize(DecodeBackend::kAuto, error_message);
}

// 初始化解码器，可以指定 requested_backend。
bool VideoDecoder::Initialize(
        DecodeBackend requested_backend,
        std::string *error_message) {

    // 先关闭已有解码器
    Shutdown();

    impl_->requested_backend = requested_backend;
    impl_->backend = CreateVideoDecoderBackend(requested_backend, error_message);
    if (!impl_->backend) {
        return false;
    }

    if (!impl_->backend->Initialize(error_message)) {
        impl_->backend.reset();
        return false;
    }

    impl_->active_backend = DecodeBackend::kSoftware;
    impl_->active_backend_name = impl_->backend->backend_name();
    return true;
}

// 解码一段编码后的视频数据。
//
// 参数：
// data：H.264 编码数据指针。
// size：H.264 数据长度。
// decoded_frame：输出的解码后帧。
// error_message：错误信息。
bool VideoDecoder::Decode(
        const std::uint8_t *data,
        std::size_t size,
        DecodedFrame *decoded_frame,
        std::string *error_message) {
    // 如果 backend 还没创建或初始化
    if (!impl_->backend) {
        if (error_message != nullptr) {
            *error_message = "decoder is not initialized";
        }
        return false;
    }
    return impl_->backend->Decode(data, size, decoded_frame, error_message);
}

// 关闭解码器，释放资源
void VideoDecoder::Shutdown() {
    if (impl_->backend) {
        impl_->backend->Shutdown();
        impl_->backend.reset();
    }

    impl_->requested_backend = DecodeBackend::kAuto;
    impl_->active_backend = DecodeBackend::kSoftware;
    impl_->active_backend_name = "software";
}

}  // namespace sclient
