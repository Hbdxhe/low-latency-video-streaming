#ifndef SSERVER_MODULES_TRANSPORT_TCP_TCPCLIENTSESSION_H
#define SSERVER_MODULES_TRANSPORT_TCP_TCPCLIENTSESSION_H

#include <atomic>
#include <cstddef>
#include <memory>
#include <mutex>
#include <string>
#include <thread>

#include "common/concurrency/ThreadSafeQueue.h"
#include "common/metrics/LatencyRecorder.h"
#include "common/model/EncodedFrame.h"
#include "config/AppConfig.h"

namespace sserver {
namespace modules {
namespace transport {
namespace tcp {

// 单个 TCP 客户端会话。
//
// TcpStreamingBackend 负责监听端口和 accept 客户端连接；
// 每 accept 到一个客户端，就会创建一个 TcpClientSession。

// TcpClientSession 负责管理“某一个客户端”的完整生命周期：
//      1. 保存客户端 socket；
//      2. 启动接收线程；
//      3. 启动发送线程；
//      4. 维护发送队列；
//      5. 将 EncodedFrame 写入 TCP socket；
//      6. 统计发送延迟、排队延迟、丢帧数量等指标；
//      7. 客户端断开后关闭 socket 并停止线程。
class TcpClientSession {
public:
    TcpClientSession(
            int socket_fd,
            const config::TransportConfig &config,
            const std::shared_ptr<common::metrics::LatencyRecorder> &send_latency_recorder,
            int latency_log_interval_frames);
    ~TcpClientSession();

    bool Start();
    void Stop();
    bool IsRunning() const;
    void EnqueueFrame(common::model::EncodedFramePtr frame);
    std::string remote_endpoint() const;

private:
    // 发送队列中的帧结构
    struct QueuedFrame {
        common::model::EncodedFramePtr frame;
        std::uint64_t enqueue_timestamp_ns = 0;
    };

    // 接收线程主循环
    void ReceiveLoop();

    // 发送线程主循环
    void SendLoop();

    // 从 socket 中接收指定长度的数据
    bool ReceiveAll(char *buffer, std::size_t length);

    // 发送一帧 EncodedFrame
    bool SendFrame(common::model::EncodedFramePtr frame, std::uint64_t send_start_timestamp_ns);

    // 分段发送一个完整消息
    bool SendMessageParts(const char *header, std::size_t header_length,
                          const char *metadata, std::size_t metadata_length,
                          const char *payload, std::size_t payload_length);
    void CloseSocket();

    // 构造客户端远端地址字符串
    std::string BuildRemoteEndpoint() const;

private:
    int socket_fd_;
    config::TransportConfig config_;
    std::atomic_bool running_;
    std::thread receive_thread_;
    std::thread send_thread_;
    std::mutex receive_mutex_;
    std::mutex send_mutex_;

    // 出站帧队列
    common::concurrency::ThreadSafeQueue<QueuedFrame> outbound_frames_;

    // 全局发送延迟统计器
    std::shared_ptr<common::metrics::LatencyRecorder> send_latency_recorder_;

    // 当前客户端自己的队列等待延迟统计器
    common::metrics::LatencyRecorder queue_wait_latency_recorder_;

    // 当前客户端自己的 socket 发送耗时统计器。
    common::metrics::LatencyRecorder send_time_latency_recorder_;

    // 延迟日志打印间隔
    int latency_log_interval_frames_;

    // 已成功发送的帧数量
    std::uint64_t sent_frames_;

    // 因发送队列溢出而丢弃的帧数量
    std::uint64_t overflow_dropped_frames_;

    // 因帧过旧而丢弃的帧数量
    std::uint64_t stale_dropped_frames_;

    // 因等待关键帧而丢弃的非关键帧数量
    std::uint64_t dropped_incoming_non_keyframes_;

    // 背压事件数量
    std::uint64_t backpressure_events_;

    // 当前观察到的最大队列深度
    std::size_t max_queue_depth_;

    // 客户端远端地址字符串
    std::string remote_endpoint_;
};

}  // namespace tcp
}  // namespace transport
}  // namespace modules
}  // namespace sserver

#endif  // SSERVER_MODULES_TRANSPORT_TCP_TCPCLIENTSESSION_H
