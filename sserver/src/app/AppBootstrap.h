#ifndef SSERVER_APP_APPBOOTSTRAP_H
#define SSERVER_APP_APPBOOTSTRAP_H

#include <memory>

#include "common/metrics/LatencyRecorder.h"
#include "config/AppConfig.h"
#include "core/Application.h"

namespace sserver {

namespace modules {
namespace capture {
    class CaptureModule;
}

namespace transport {
    class TransportModule;
}
}  // namespace modules

namespace app {

// AppBootstrap 是 sserver 的“装配器”或“组合根”。
//
// main.cpp 故意保持很薄：只负责加载配置、注册信号、启动/停止进程。
// 真正把服务端模块拼成一条视频流水线的工作，放在 AppBootstrap 里完成。
//
// 服务端核心流水线是：
//
//   CaptureModule
//       -> 产出 EncodedFrame
//       -> 触发帧回调
//       -> TransportModule
//
// 这样设计有两个好处：
//   1. main.cpp 不需要知道每个模块的内部细节。
//   2. CaptureModule、TransportModule、Application 可以被测试和 benchmark 复用。
class AppBootstrap {
public:
    explicit AppBootstrap(const config::AppConfig &config);

    // 析构函数里会调用 Stop()，作为兜底保护。
    // 即使调用方忘记显式停止，也尽量不泄漏后台线程、socket 或摄像头句柄。
    ~AppBootstrap();

    // 创建模块，注册到 Application，并执行各模块的 initialize()。
    //
    // 这个函数是“幂等”的：成功初始化后再次调用，不会重复创建模块。
    // 幂等性让 Start() 可以放心先调用 Initialize()。
    bool Initialize();

    // 启动服务端流水线。
    //
    // 内部会先确保 Initialize() 已完成，然后绑定“采集 -> 传输”的回调，
    // 最后启动所有注册模块。这个函数返回 true 后，采集和传输相关线程
    // 可能已经开始运行。
    bool Start();

    // 停止服务端流水线。
    //
    // 先解除采集到传输的回调，再停止模块，最后调用 shutdown 钩子。
    // 可以被多次调用。
    void Stop();

    // 暴露 Application，主要给测试代码检查模块生命周期状态使用。
    // 生产环境的 main.cpp 通常不需要直接操作 Application。
    core::Application &application();

    // 服务端发送延迟统计器。
    //
    // 传输后端会用它统计 capture_to_send 等指标，用于日志和 benchmark。
    std::shared_ptr<common::metrics::LatencyRecorder> send_latency_recorder() const;

    // 返回传输模块实际绑定的端口。
    //
    // 当配置 listen_port=0 时，端口由操作系统自动分配，测试代码可以通过
    // 这个函数拿到真实端口。
    int bound_port() const;

private:
    // 连接 CaptureModule 的输出和 TransportModule 的输入。
    void BindStreamingPipeline();

    // 解除帧回调，避免 shutdown 过程中采集模块继续调用传输模块。
    void UnbindStreamingPipeline();

    void PrintStartupSummary() const;
    void PrintShutdownSummary() const;

    // 加载后的配置副本。
    //
    // 模块会通过 ApplicationContext 读取配置；AppBootstrap 自己保留一份，
    // 用于构造 Application 和打印启动摘要。
    config::AppConfig config_;

    // Application 负责模块注册表和生命周期编排：
    // initialize -> start -> stop -> shutdown。
    std::unique_ptr<core::Application> application_;

    // shared_ptr 的原因：它既传给 TransportModule 使用，也可能被测试/benchmark
    // 通过 AppBootstrap 读取。
    std::shared_ptr<common::metrics::LatencyRecorder> send_latency_recorder_;

    // 服务端的两个核心模块。
    //
    // 它们注册给 Application 管生命周期，同时 AppBootstrap 也需要持有它们，
    // 因为要调用模块特有接口来绑定帧回调。
    std::shared_ptr<modules::capture::CaptureModule> capture_module_;
    std::shared_ptr<modules::transport::TransportModule> transport_module_;

    // 记录 Application::Start() 是否成功。
    // Stop() 借此判断是否需要打印“application stopped”之类的停止摘要。
    bool started_;
};

}  // namespace app
}  // namespace sserver

#endif  // SSERVER_APP_APPBOOTSTRAP_H
