#ifndef SSERVER_MODULES_CAPTURE_VIDEO_CAPTURE_H
#define SSERVER_MODULES_CAPTURE_VIDEO_CAPTURE_H

#include <memory>
#include <string>

#include "common/model/EncodedFrame.h"
#include "config/AppConfig.h"
#include "core/ApplicationContext.h"
#include "core/ModuleState.h"
#include "modules/capture/video/ICaptureDevice.h"

namespace sserver {
namespace modules {
namespace capture {

// 采集后端枚举。
// 配置 capture.source 会映射为 null 或 v4l2；kAuto 是预留的自动选择模式。
enum class CaptureBackend {
    kAuto,
    kNull,
    kV4L2,
};

// 后端选择结果：枚举用于代码分支，名称用于日志和诊断。
struct CaptureBackendSelection {
    CaptureBackend backend = CaptureBackend::kAuto;
    std::string backend_name = "auto";
};

// 将 CaptureConfig::source 解析成内部后端类型。
// AppConfig 校验和 Capture 初始化共用这套规则，避免支持列表不一致。
inline bool ResolveCaptureBackendSelection(
        const config::CaptureConfig &config,
        CaptureBackendSelection *selection,
        std::string *error_message) {
    // 调用方必须提供输出对象。
    if (selection == nullptr) {
        if (error_message != nullptr) {
            *error_message = "capture backend selection output is null";
        }
        return false;
    }

    // Null 后端不依赖真实摄像头，主要用于测试和 benchmark。
    if (config.source == "null") {
        selection->backend = CaptureBackend::kNull;
        selection->backend_name = "null";
        return true;
    }

    // V4L2 后端通过 /dev/videoX 采集真实摄像头画面。
    if (config.source == "v4l2") {
        selection->backend = CaptureBackend::kV4L2;
        selection->backend_name = "v4l2";
        return true;
    }

    // 未知 source 直接失败，避免拼写错误悄悄使用默认后端。
    if (error_message != nullptr) {
        *error_message = "capture.source must be either 'v4l2' or 'null'";
    }
    return false;
}

// 采集后端工厂：根据选择结果创建 NullCaptureDevice 或 V4L2CaptureDevice。
class CaptureBackendFactory {
public:
    static std::unique_ptr<ICaptureDevice> Create(
            const CaptureBackendSelection &selection,
            const config::CaptureConfig &capture_config,
            const config::CodecConfig &codec_config,
            std::string *error_message);
};

// 采集子系统门面。
//
// CaptureModule 只操作 Capture，不直接依赖具体设备：
//   CaptureModule -> Capture -> ICaptureDevice -> Null/V4L2
//
// Capture 负责后端选择、设备创建、生命周期转发和单双线程接口转发。
class Capture {
public:
    Capture();
    ~Capture();

    // 解析后端并创建具体设备，但尚不打开设备。
    bool initialize(const core::ApplicationContext &context, std::string *error_message);
    // 打开并启动具体设备。
    bool start(std::string *error_message);
    // 停止活动采集并关闭设备。
    void stop();
    // 最终清理并销毁设备后端。
    void shutdown();
    core::ModuleState state() const;

    // 单线程路径：直接获取编码后的帧。
    common::model::EncodedFramePtr CaptureFrame();
    CaptureBackend backend() const;
    const std::string &backend_name() const;
    std::string Describe() const;

    // 双线程路径：采集原始帧，再在另一线程编码。
    bool SupportsRawCapture() const;
    RawCaptureFramePtr CaptureRawFrame();
    common::model::EncodedFramePtr EncodeRawFrame(RawCaptureFramePtr raw);

private:
    // pimpl：把具体设备和后端状态隐藏在 .cpp 中。
    struct CaptureImpl;
    std::unique_ptr<CaptureImpl> impl_;
};

}  // namespace capture
}  // namespace modules
}  // namespace sserver

#endif  // SSERVER_MODULES_CAPTURE_VIDEO_CAPTURE_H
