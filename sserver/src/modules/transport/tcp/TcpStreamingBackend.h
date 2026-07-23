#ifndef SSERVER_MODULES_TRANSPORT_TCP_TCPSTREAMINGBACKEND_H
#define SSERVER_MODULES_TRANSPORT_TCP_TCPSTREAMINGBACKEND_H

#include <atomic>
#include <memory>
#include <mutex>
#include <thread>
#include <vector>

#include "common/metrics/LatencyRecorder.h"
#include "config/AppConfig.h"
#include "modules/transport/ITransportBackend.h"
#include "modules/transport/tcp/TcpClientSession.h"

namespace sserver {
namespace modules {
namespace transport {
namespace tcp {

class TcpStreamingBackend : public ITransportBackend {
public:
    explicit TcpStreamingBackend(const std::shared_ptr<common::metrics::LatencyRecorder> &send_latency_recorder);
    ~TcpStreamingBackend() override;

    bool initialize(const core::ApplicationContext &context) override;
    bool start() override;
    void stop() override;
    void shutdown() override;
    core::ModuleState state() const override;
    void Broadcast(common::model::EncodedFramePtr frame) override;
    int bound_port() const override;
    TransportBackend backend() const override;
    const std::string &backend_name() const override;

private:
    // 创建、绑定并监听 TCP socket
    bool OpenListenSocket();

    // 关闭监听 socket
    void CloseListenSocket();

    // accept 线程主循环
    void AcceptLoop();

    // 清理已经关闭的客户端连接
    void PruneClosedClients();

private:
    // 传输配置
    config::TransportConfig config_;

    // 后端名称
    std::string backend_name_;

    // 延迟日志输出间隔
    int latency_log_interval_frames_;

    // TCP 监听 socket 文件描述符
    int listen_socket_fd_;

    // 实际绑定端口
    int bound_port_;

    // 运行标志
    std::atomic_bool running_;

    // 接收客户端连接的线程
    std::thread accept_thread_;

    // 客户端列表互斥锁
    std::mutex clients_mutex_;

    // 当前已连接的 TCP 客户端会话列表
    std::vector<std::shared_ptr<TcpClientSession>> clients_;

    // 发送延迟统计器
    std::shared_ptr<common::metrics::LatencyRecorder> send_latency_recorder_;

    // 当前 TCP 后端状态
    std::atomic<core::ModuleState> state_;
};

}  // namespace tcp
}  // namespace transport
}  // namespace modules
}  // namespace sserver

#endif  // SSERVER_MODULES_TRANSPORT_TCP_TCPSTREAMINGBACKEND_H
