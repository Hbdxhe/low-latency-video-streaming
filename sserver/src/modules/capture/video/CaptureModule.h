#ifndef SSERVER_MODULES_CAPTURE_VIDEO_CAPTUREMODULE_H
#define SSERVER_MODULES_CAPTURE_VIDEO_CAPTUREMODULE_H

#include <atomic>
#include <functional>
#include <memory>
#include <mutex>
#include <thread>

#include "common/concurrency/ThreadSafeQueue.h"
#include "common/model/EncodedFrame.h"
#include "core/IModule.h"
#include "modules/capture/video/Capture.h"
#include "modules/capture/video/ICaptureDevice.h"

namespace sserver {
namespace modules {
namespace capture {

// 编码帧回调。AppBootstrap 用它把 CaptureModule 输出连接到 TransportModule。
using FrameHandler = std::function<void(common::model::EncodedFramePtr)>;

// 采集模块的生命周期和线程调度层。
//
// 它负责接入 IModule、管理采集/编码线程、维护原始帧队列，
// 最终通过 FrameHandler 向下游输出 EncodedFrame。
class CaptureModule : public core::IModule {
public:
    CaptureModule();
    ~CaptureModule() override;

    std::string name() const override;
    bool initialize(const core::ApplicationContext &context) override;
    bool start() override;
    void stop() override;
    void shutdown() override;
    core::ModuleState state() const override;

    // 设置编码帧下游回调。
    void SetFrameHandler(FrameHandler handler);

private:
    // 单线程路径：设备内部完成采集和编码。
    void CaptureLoop();
    // 双线程路径的采集线程：快速取原始帧并入队。
    void CapturePump();
    // 双线程路径的编码线程：出队并编码。
    void EncodePump();

private:
    // 采集门面，内部选择 Null 或 V4L2。
    std::unique_ptr<Capture> capture_;
    // 单线程时运行 CaptureLoop，双线程时运行 CapturePump。
    std::thread worker_thread_;
    // 双线程时运行 EncodePump。
    std::thread encode_thread_;
    // 控制工作线程循环退出。
    std::atomic_bool running_;
    // 模块生命周期状态。
    std::atomic<core::ModuleState> state_;
    // 保护 frame_handler_ 的跨线程读写。
    std::mutex handler_mutex_;
    // 编码帧输出回调。
    FrameHandler frame_handler_;
    // 采集线程和编码线程之间的原始帧队列。
    common::concurrency::ThreadSafeQueue<RawCaptureFramePtr> raw_queue_;
    // 当前设备是否支持 raw capture 双线程路径。
    bool use_raw_capture_;
};

}  // namespace capture
}  // namespace modules
}  // namespace sserver

#endif  // SSERVER_MODULES_CAPTURE_VIDEO_CAPTUREMODULE_H
