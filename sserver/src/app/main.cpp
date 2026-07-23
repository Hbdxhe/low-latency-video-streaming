#include <atomic>
#include <chrono>
#include <csignal>
#include <cstring>
#include <string>
#include <thread>

#include "app/AppBootstrap.h"
#include "common/log/Logger.h"
#include "config/AppConfig.h"

namespace {

// 进程级退出标记。
//
// 信号处理函数里应该尽量少做事，因为它可能在任意线程、任意时刻被触发。
// 这里采用最稳妥的做法：信号处理函数只修改一个 atomic 标记，真正的停止流程
// 交给 main 线程完成。
std::atomic_bool g_exit_requested(false);

void OnSignal(int signal_number) {
    (void) signal_number;
    g_exit_requested = true;
}

// 解析服务端入口当前支持的命令行参数：
//   --config <配置文件路径>
//
// 如果没有传 --config，就默认读取 config/default.conf。
// 注意：这里的相对路径是相对于“当前运行目录”的，不是相对于可执行文件位置。
// 所以你之前在 sserver/build 目录下运行时，config/null_tcp.conf 会被解析成
// sserver/build/config/null_tcp.conf，从而找不到文件；回到 sserver 根目录运行就正常。
std::string ParseConfigPath(int argc, char **argv) {
    std::string config_path = "config/default.conf";
    for (int index = 1; index < argc; ++index) {
        if (std::strcmp(argv[index], "--config") == 0 && index + 1 < argc) {
            config_path = argv[index + 1];
            ++index;
        }
    }
    return config_path;
}

}  // namespace

int main(int argc, char **argv) {
    // 第 1 步：确定本次运行使用哪个 .conf 配置文件。
    //
    // 常见配置包括：
    //   config/null_tcp.conf              使用测试视频源 + TCP
    //   config/null_udp.conf              使用测试视频源 + UDP
    //   config/sclient_tcp.conf           使用真实摄像头 + TCP
    //   config/sclient_udp.conf           使用真实摄像头 + UDP
    //   config/integration_rtp_v4l2.conf  使用真实摄像头 + RTP
    const std::string config_path = ParseConfigPath(argc, argv);

    // 第 2 步：加载并校验运行配置。
    //
    // AppConfig 是整个服务端的总配置对象，里面分成几大块：
    //   capture   采集配置，比如 v4l2/null、/dev/video0、宽高帧率
    //   codec     编码配置，比如 x264 preset/tune/profile
    //   transport 传输配置，比如 tcp/udp/rtp、端口、NACK/FEC
    //   runtime   运行时配置，比如退出等待时间、日志统计周期
    //
    // 如果这里失败，通常是配置路径不对，或者配置项的值没有通过 Validate() 校验。
    sserver::config::AppConfig config;
    std::string error_message;
    if (!sserver::config::ConfigLoader::LoadFromFile(config_path, &config, &error_message)) {
        sserver::common::log::Logger::Error("failed to load config: " + error_message);
        return 1;
    }

    // 日志模块会生成本次运行的日志文件路径。
    // 这里尽早打印出来，方便你之后查看服务端详细的延迟统计、队列统计、传输状态。
    const std::string log_file_path = sserver::common::log::Logger::CurrentLogFilePath();
    if (!log_file_path.empty()) {
        sserver::common::log::Logger::Info("log file: " + log_file_path);
    }
    sserver::common::log::Logger::Info("loading config: " + config_path);

    // 第 3 步：注册进程信号处理。
    //
    // SIGINT  ：终端里按 Ctrl+C。
    // SIGTERM ：外部请求进程正常退出。
    // SIGPIPE ：向已经断开的 socket 写数据时可能触发。
    //
    // 对流媒体服务来说，客户端断开连接很正常。这里忽略 SIGPIPE，避免服务端因为
    // 某个客户端断开而被系统直接杀掉；真正的发送失败由 TCP/UDP/RTP 后端自己处理。
    std::signal(SIGINT, OnSignal);
    std::signal(SIGTERM, OnSignal);
    std::signal(SIGPIPE, SIG_IGN);

    // 第 4 步：创建并启动真正的媒体流水线。
    //
    // AppBootstrap 是服务端“装配器”，它负责把模块接起来：
    //   CaptureModule 产出 EncodedFrame
    //       -> 触发帧回调
    //       -> TransportModule::Broadcast(frame)
    //
    // main.cpp 不直接关心摄像头、编码器、TCP/UDP/RTP 的细节。
    sserver::app::AppBootstrap bootstrap(config);
    if (!bootstrap.Start()) {
        sserver::common::log::Logger::Error("application failed to start");
        return 1;
    }

    // 第 5 步：让主线程保持存活，直到收到退出信号。
    //
    // 真正干活的是模块内部线程，例如：
    //   采集线程
    //   编码线程
    //   TCP accept/send 线程
    //   UDP/RTP 发送逻辑
    //
    // main 线程在这里更像“进程管理员”：活着等待，收到退出信号后负责收尾。
    while (!g_exit_requested.load()) {
        std::this_thread::sleep_for(std::chrono::milliseconds(200));
    }

    // 第 6 步：优雅停止服务端。
    //
    // Stop() 会解除模块之间的回调关系，停止后台线程，释放摄像头和 socket 资源。
    sserver::common::log::Logger::Info("shutdown requested by signal");
    bootstrap.Stop();
    return 0;
}
