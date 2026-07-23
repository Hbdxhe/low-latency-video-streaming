#include "modules/capture/video/Capture.h"

#include <utility>

#include "common/log/Logger.h"
#include "modules/capture/video/ICaptureDevice.h"
#include "modules/capture/video/null/NullCaptureDevice.h"
#include "modules/capture/video/v4l2/V4L2CaptureDevice.h"

namespace sserver {
namespace modules {
namespace capture {

// Capture 的私有实现。
//
// 这里使用 pimpl 风格，把状态和具体设备对象藏在 .cpp 内部：
//   头文件暴露稳定接口
//   .cpp 内部自由维护 backend、device、state 等细节
struct Capture::CaptureImpl {
    // 采集相关配置，例如 source/device/width/height/fps。
    config::CaptureConfig capture_config;
    // 编码相关配置，因为真实采集设备内部会创建 VideoEncoder。
    config::CodecConfig codec_config;
    // 用户请求的采集后端，例如 auto/null/v4l2。
    CaptureBackend requested_backend = CaptureBackend::kAuto;
    std::string requested_backend_name = "auto";
    // 实际启用的采集后端。
    CaptureBackend active_backend = CaptureBackend::kNull;
    std::string active_backend_name = "null";
    // 具体采集设备。可能是 NullCaptureDevice，也可能是 V4L2CaptureDevice。
    std::unique_ptr<ICaptureDevice> device;
    core::ModuleState state = core::ModuleState::kCreated;
};

std::unique_ptr<ICaptureDevice> CaptureBackendFactory::Create(
        const CaptureBackendSelection &selection,
        const config::CaptureConfig &capture_config,
        const config::CodecConfig &codec_config,
        std::string *error_message) {
    switch (selection.backend) {
        case CaptureBackend::kNull:
            // null 后端用于无摄像头测试、协议测试、benchmark。
            return std::unique_ptr<ICaptureDevice>(new NullCaptureDevice(capture_config, codec_config));
        case CaptureBackend::kV4L2:
            // v4l2 后端用于真实 Linux 摄像头，例如 /dev/video0。
            return std::unique_ptr<ICaptureDevice>(new v4l2::V4L2CaptureDevice(capture_config, codec_config));
        case CaptureBackend::kAuto:
        default:
            if (error_message != nullptr) {
                *error_message = "requested capture backend is not supported";
            }
            return nullptr;
    }
}

Capture::Capture()
        : impl_(std::make_unique<CaptureImpl>()) {
}

Capture::~Capture() {
    shutdown();
}

bool Capture::initialize(const core::ApplicationContext &context, std::string *error_message) {
    // 重新初始化前先清理旧设备，保证 initialize 可重复调用。
    shutdown();

    // 根据 context.config.capture.source 解析采集后端。
    // 支持情况由 ResolveCaptureBackendSelection() 统一判断。
    CaptureBackendSelection selection;
    if (!ResolveCaptureBackendSelection(context.config.capture, &selection, error_message)) {
        impl_->state = core::ModuleState::kFailed;
        return false;
    }

    // 保存配置和后端选择结果，后续 start()/Describe()/backend_name() 会用到。
    impl_->capture_config = context.config.capture;
    impl_->codec_config = context.config.codec;
    impl_->requested_backend = selection.backend;
    impl_->requested_backend_name = selection.backend_name;
    // 创建具体设备对象，但此时还不打开摄像头。
    // 真实 Open()/Start() 会在 Capture::start() 阶段发生。
    impl_->device = CaptureBackendFactory::Create(selection, context.config.capture, context.config.codec, error_message);
    if (!impl_->device) {
        impl_->state = core::ModuleState::kFailed;
        return false;
    }

    impl_->active_backend = selection.backend;
    impl_->active_backend_name = selection.backend_name;
    impl_->state = core::ModuleState::kInitialized;
    return true;
}

bool Capture::start(std::string *error_message) {
    // 如果配置关闭采集模块，则直接进入 Running。
    // 这种模式常用于测试生命周期，不产生实际帧。
    if (!impl_->capture_config.enabled) {
        impl_->state = core::ModuleState::kRunning;
        return true;
    }

    // start 阶段要求 initialize 已经成功创建 device。
    if (!impl_->device) {
        impl_->state = core::ModuleState::kFailed;
        if (error_message != nullptr) {
            *error_message = "capture is not initialized";
        }
        return false;
    }

    // 打开设备。
    // V4L2：open(/dev/video0)
    // Null：标记 opened
    if (!impl_->device->Open()) {
        impl_->state = core::ModuleState::kFailed;
        if (error_message != nullptr && error_message->empty()) {
            *error_message = "failed to open capture device";
        }
        return false;
    }

    // 启动设备。
    // V4L2：配置格式/帧率/mmap buffer/x264/STREAMON
    // Null：准备文本 payload 或合成 YUYV 测试图案
    if (!impl_->device->Start()) {
        impl_->device->Close();
        impl_->state = core::ModuleState::kFailed;
        if (error_message != nullptr && error_message->empty()) {
            *error_message = "failed to start capture device";
        }
        return false;
    }

    impl_->state = core::ModuleState::kRunning;
    return true;
}

void Capture::stop() {
    if (!impl_->device) {
        impl_->state = core::ModuleState::kStopped;
        return;
    }

    // 停止活动采集并关闭设备句柄。
    // Stop/Close 分开，是为了匹配常见设备生命周期：先停流，再关 fd。
    impl_->device->Stop();
    impl_->device->Close();
    impl_->state = core::ModuleState::kStopped;
}

void Capture::shutdown() {
    // shutdown 是最终清理：停止设备、关闭句柄、销毁具体后端对象。
    if (impl_->device) {
        impl_->device->Stop();
        impl_->device->Close();
        impl_->device.reset();
    }

    impl_->requested_backend = CaptureBackend::kAuto;
    impl_->requested_backend_name = "auto";
    impl_->active_backend = CaptureBackend::kNull;
    impl_->active_backend_name = "null";
    impl_->state = core::ModuleState::kShutdown;
}

core::ModuleState Capture::state() const {
    return impl_->state;
}

common::model::EncodedFramePtr Capture::CaptureFrame() {
    // 单线程采集路径：让设备直接返回 EncodedFrame。
    if (!impl_->device) {
        return common::model::EncodedFramePtr();
    }
    return impl_->device->CaptureFrame();
}

CaptureBackend Capture::backend() const {
    return impl_->active_backend;
}

const std::string &Capture::backend_name() const {
    return impl_->active_backend_name;
}

std::string Capture::Describe() const {
    if (!impl_->device) {
        return "capture(uninitialized)";
    }
    return impl_->device->Describe();
}

bool Capture::SupportsRawCapture() const {
    // 双线程路径能力由具体设备决定。
    // 当前 V4L2 支持，Null 不支持。
    return impl_->device && impl_->device->SupportsRawCapture();
}

RawCaptureFramePtr Capture::CaptureRawFrame() {
    // 双线程路径的采集阶段：只取原始帧。
    if (!impl_->device) {
        return nullptr;
    }
    return impl_->device->CaptureRawFrame();
}

common::model::EncodedFramePtr Capture::EncodeRawFrame(RawCaptureFramePtr raw) {
    // 双线程路径的编码阶段：把原始帧编码成 EncodedFrame。
    if (!impl_->device) {
        return nullptr;
    }
    return impl_->device->EncodeRawFrame(raw);
}

}  // namespace capture
}  // namespace modules
}  // namespace sserver
