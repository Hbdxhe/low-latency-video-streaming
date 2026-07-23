#include "modules/encoding/video/VideoEncoder.h"

#include <utility>

#include "modules/encoding/video/VideoEncoderBackend.h"
#include "modules/encoding/video/x264/X264VideoEncoderBackend.h"

namespace sserver {
namespace modules {
namespace encoding {

// VideoEncoder 的私有实现结构体。
//
// 这是典型的 PImpl 写法：
// VideoEncoder.h 中只暴露 std::unique_ptr<VideoEncoderImpl>，真正的数据成员和实现细节都放在 .cpp 中。
struct VideoEncoder::VideoEncoderImpl {
    // 用户请求的编码后端，例如 auto/x264。
    EncodeBackend requested_backend = EncodeBackend::kAuto;
    std::string requested_backend_name = "auto";

    // 实际启用的编码后端。
    EncodeBackend active_backend = EncodeBackend::kX264;
    std::string active_backend_name = "x264";

    // 具体编码器后端对象。
    std::unique_ptr<VideoEncoderBackend> backend;
};


// 根据后端选择结果创建具体编码器后端对象。
//
// 当前只支持 x264 后端。
// 后续如果支持硬件编码，可以在这里扩展：
//      EncodeBackend::kNvenc
//      EncodeBackend::kVaapi
//      EncodeBackend::kQsv
std::unique_ptr<VideoEncoderBackend> VideoEncoderBackendFactory::Create(
        const VideoEncoderBackendSelection &selection,
        std::string *error_message) {

    switch (selection.backend) {
        case EncodeBackend::kX264:
            // 当前软件编码后端：x264。
            return CreateX264VideoEncoderBackend();
        case EncodeBackend::kAuto:
        default:
            if (error_message != nullptr) {
                *error_message = "requested encode backend is not supported";
            }
            return nullptr;
    }
}

VideoEncoder::VideoEncoder()
        : impl_(std::make_unique<VideoEncoderImpl>()) {
}

VideoEncoder::~VideoEncoder() {
    Shutdown();
}

bool VideoEncoder::Initialize(int width, int height, int fps, const config::CodecConfig &config) {
    return Initialize(width, height, fps, config, nullptr);
}

bool VideoEncoder::Initialize(
        int width,
        int height,
        int fps,
        const config::CodecConfig &config,
        std::string *error_message) {
    // 重新初始化前先清理旧 backend，保证 Initialize 可重复调用。
    Shutdown();

    // 根据 codec.backend 解析编码后端。
    VideoEncoderBackendSelection selection;
    if (!ResolveVideoEncoderBackendSelection(config, &selection, error_message)) {
        return false;
    }

    impl_->requested_backend = selection.backend;
    impl_->requested_backend_name = selection.backend_name;
    // 创建具体编码后端，但真正分配 x264 资源发生在 backend->Initialize()。
    impl_->backend = VideoEncoderBackendFactory::Create(selection, error_message);
    if (!impl_->backend) {
        return false;
    }

    // 把分辨率、帧率和 x264 参数传给后端。
    if (!impl_->backend->Initialize(width, height, fps, config, error_message)) {
        impl_->backend.reset();
        return false;
    }

    impl_->active_backend = impl_->backend->backend();
    impl_->active_backend_name = impl_->backend->backend_name();
    return true;
}

bool VideoEncoder::EncodeYuyv422Frame(
        const std::uint8_t *input,
        std::size_t input_length,
        std::vector<std::uint8_t> *output,
        bool *is_keyframe) {
    return EncodeYuyv422Frame(input, input_length, output, is_keyframe, nullptr);
}

bool VideoEncoder::EncodeYuyv422Frame(
        const std::uint8_t *input,
        std::size_t input_length,
        std::vector<std::uint8_t> *output,
        bool *is_keyframe,
        std::string *error_message) {
    // 所有采集设备统一把 YUYV422 原始帧交给 VideoEncoder。
    // VideoEncoder 再委托给具体 backend 完成格式转换和 H.264 编码。
    if (!impl_->backend) {
        if (error_message != nullptr) {
            *error_message = "video encoder is not initialized";
        }
        return false;
    }

    return impl_->backend->EncodeYuyv422Frame(input, input_length, output, is_keyframe, error_message);
}

void VideoEncoder::Shutdown() {
    // 释放具体编码器后端，例如关闭 x264 encoder、释放 picture/param。
    if (impl_->backend) {
        impl_->backend->Shutdown();
        impl_->backend.reset();
    }

    impl_->requested_backend = EncodeBackend::kAuto;
    impl_->requested_backend_name = "auto";
    impl_->active_backend = EncodeBackend::kX264;
    impl_->active_backend_name = "x264";
}

EncodeBackend VideoEncoder::backend() const {
    return impl_->active_backend;
}

const std::string &VideoEncoder::backend_name() const {
    return impl_->active_backend_name;
}

std::unique_ptr<IVideoEncoder> CodecFactory::Create(const config::CodecConfig &config) {
    // 保留给更通用的编码器工厂接口使用。
    // 当前校验后返回 VideoEncoder 门面对象。
    VideoEncoderBackendSelection selection;
    if (!ResolveVideoEncoderBackendSelection(config, &selection, nullptr)) {
        return std::unique_ptr<IVideoEncoder>();
    }
    return std::unique_ptr<IVideoEncoder>(new VideoEncoder());
}

}  // namespace encoding
}  // namespace modules
}  // namespace sserver
