#include "app/AppBootstrap.h"

#include <sstream>

#include "common/log/Logger.h"
#include "modules/capture/video/CaptureModule.h"
#include "modules/transport/TransportModule.h"

namespace sserver {
namespace app {

AppBootstrap::AppBootstrap(const config::AppConfig &config)
        // 把解析后的配置复制进 AppBootstrap。
        //
        // config_ 的生命周期覆盖整个服务端运行过程。Application 构造时拿到
        // config_ 的引用，之后各模块通过 ApplicationContext 读取同一份配置。
        : config_(config),
          application_(new core::Application(config_)),
          send_latency_recorder_(std::make_shared<common::metrics::LatencyRecorder>()),
          started_(false) {
}

AppBootstrap::~AppBootstrap() {
    // 析构时兜底停止。
    //
    // Stop() 是幂等的，即使 main.cpp 已经手动调用过 Stop()，这里再次调用也安全。
    // 这样可以降低后台线程、socket、摄像头句柄泄漏的风险。
    Stop();
}

bool AppBootstrap::Initialize() {
    // 如果 capture_module_ 已经存在，说明模块已经创建并注册过。
    // 直接返回 true，避免重复注册模块。
    if (capture_module_ != nullptr) {
        return true;
    }

    // 先创建传输模块，再创建采集模块。
    //
    // 注册顺序很重要：Application 会按注册顺序启动模块。
    // 对服务端来说，最好让传输模块先准备好监听/发送能力，再启动采集模块产帧。
    // 否则采集模块可能已经开始产出 EncodedFrame，但传输端还没有准备好。
    transport_module_ = std::make_shared<modules::transport::TransportModule>(send_latency_recorder_);
    capture_module_ = std::make_shared<modules::capture::CaptureModule>();

    // Application 负责生命周期编排，但 AppBootstrap 仍保留 shared_ptr。
    // 因为 AppBootstrap 还要调用 CaptureModule::SetFrameHandler() 这类模块专属接口。
    application_->RegisterModule(transport_module_);
    application_->RegisterModule(capture_module_);

    // 调用所有已注册模块的 initialize(context)。
    //
    // 每个模块会从 ApplicationContext 中读取自己关心的配置：
    //   CaptureModule 读取 capture/codec
    //   TransportModule 读取 transport
    return application_->Initialize();
}

bool AppBootstrap::Start() {
    if (!Initialize()) {
        return false;
    }
    if (started_) {
        return true;
    }

    // 在线程启动前，先搭好数据通路。
    //
    // CaptureModule 一旦 start，就可能立刻开始产出 EncodedFrame 并调用回调。
    // 所以必须先 BindStreamingPipeline()，再 application_->Start()。
    BindStreamingPipeline();

    // 启动所有已注册模块。
    //
    // 这一步之后，传输监听循环、采集线程、编码线程等都可能在后台并发运行。
    started_ = application_->Start();
    if (started_) {
        PrintStartupSummary();
    } else {
        // 如果某个模块启动失败，解除回调，避免出现“半启动”的流水线：
        // 例如采集端仍然可能引用传输端，但传输端其实没有正常启动。
        UnbindStreamingPipeline();
    }
    return started_;
}

void AppBootstrap::Stop() {
    if (application_ == nullptr) {
        return;
    }

    const bool was_started = started_;

    // 停止前先断开数据通路。
    //
    // 这是一个 shutdown 顺序保护：当传输后端正在关闭 socket 时，不应该再有新帧
    // 从 CaptureModule 交给 TransportModule。
    UnbindStreamingPipeline();

    // Stop 负责停止活动工作，例如通知线程退出、join 线程、关闭正在使用的资源。
    // Shutdown 负责最终资源释放，并把模块推进到终止状态。
    application_->Stop();
    application_->Shutdown();
    if (was_started) {
        PrintShutdownSummary();
    }
    started_ = false;
}

core::Application &AppBootstrap::application() {
    // 测试代码会用这个接口检查注册模块和生命周期状态。
    // 生产环境的 main.cpp 不需要直接访问 Application。
    return *application_;
}

std::shared_ptr<common::metrics::LatencyRecorder> AppBootstrap::send_latency_recorder() const {
    return send_latency_recorder_;
}

int AppBootstrap::bound_port() const {
    // Initialize() 之前还没有 transport_module_，自然也没有绑定端口。
    // 返回 0 表示“未知/尚未绑定”。
    if (transport_module_ == nullptr) {
        return 0;
    }
    return transport_module_->bound_port();
}

void AppBootstrap::BindStreamingPipeline() {
    if (capture_module_ == nullptr || transport_module_ == nullptr) {
        return;
    }

    // 这个 lambda 就是服务端最核心的数据通路：
    //
    //   采集/编码模块产出 EncodedFrame
    //       -> 调用这里绑定的回调
    //       -> TransportModule::Broadcast(frame)
    //       -> 具体 TCP/UDP/RTP 后端发送
    //
    // CaptureModule 不需要知道当前用的是 TCP、UDP 还是 RTP。
    // 它只负责产出 EncodedFrame；TransportModule 负责隐藏后端差异。
    capture_module_->SetFrameHandler(
            [transport_module = transport_module_](common::model::EncodedFramePtr frame) {
                transport_module->Broadcast(frame);
            });
}

void AppBootstrap::UnbindStreamingPipeline() {
    if (capture_module_ == nullptr) {
        return;
    }

    // 用空回调替换原来的回调。
    // 这样在启动失败或关闭过程中，即使采集线程晚到了一帧，也不会继续进入传输模块。
    capture_module_->SetFrameHandler(modules::capture::FrameHandler());
}

void AppBootstrap::PrintStartupSummary() const {
    // 打印一行启动摘要，方便终端快速确认本次运行模式。
    // 更细的逐帧延迟、队列状态等统计由下层模块在运行期间输出。
    std::ostringstream stream;
    stream << "application started: app=" << config_.app_name
           << ", capture_source=" << config_.capture.source
           << ", transport_backend=" << config_.transport.backend
           << ", listen_port=" << config_.transport.listen_port;
    common::log::Logger::Info(stream.str());
}

void AppBootstrap::PrintShutdownSummary() const {
    common::log::Logger::Info("application stopped");
}

}  // namespace app
}  // namespace sserver
