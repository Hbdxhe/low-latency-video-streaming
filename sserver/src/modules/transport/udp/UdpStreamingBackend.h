#ifndef SSERVER_MODULES_TRANSPORT_UDP_UDPSTREAMINGBACKEND_H
#define SSERVER_MODULES_TRANSPORT_UDP_UDPSTREAMINGBACKEND_H

#include <atomic>
#include <cstdint>
#include <deque>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include <netinet/in.h>

#include "common/metrics/LatencyRecorder.h"
#include "common/net/StreamProtocol.h"
#include "config/AppConfig.h"
#include "modules/transport/ITransportBackend.h"

namespace sserver {
namespace modules {
namespace transport {
namespace udp {

// UDP 流式传输后端。
//
// UdpStreamingBackend 是 ITransportBackend 的具体实现，
// 负责通过 UDP 向客户端发送编码后的音视频帧。
//
// 与 TCP 后端不同：
// 1. UDP 是无连接协议，没有 accept()；
// 2. 服务端通过客户端发来的 keepalive 注册客户端地址；
// 3. 服务端后续通过 sendto() / sendmmsg() 向这些客户端地址发送数据；
// 4. 大帧需要拆成多个 UDP 分片；
// 5. 可以支持 FEC 前向纠错和 NACK 重传。

class UdpStreamingBackend : public ITransportBackend {
public:
    explicit UdpStreamingBackend(const std::shared_ptr<common::metrics::LatencyRecorder> &send_latency_recorder);
    ~UdpStreamingBackend() override;

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
    // UDP 客户端端点信息。
    //
    // UDP 没有连接状态，所以服务端需要自己维护“哪些客户端还活着”。
    // 客户端通过周期性 keepalive 报文告诉服务端自己的地址。
    struct UdpClientEndpoint {
        // 客户端地址，包含 IP 和端口
        sockaddr_in address;

        // 最近一次收到该客户端 keepalive 或 NACK 的时间
        std::uint64_t last_seen_ns;

        // 客户端最近一次上报的接收统计信息
        common::net::UdpReceiverReport latest_report;

        // 最近一次收到 receiver report 的时间
        std::uint64_t latest_report_timestamp_ns;

        // 是否已经收到过 receiver report
        bool has_report;
    };

    // 缓存的 UDP 分片，用于 NACK 重传。
    // 当客户端报告某个分片丢失时，服务端可以从缓存中找到该分片并重发。
    struct CachedUdpFragment {
        // 分片编号
        std::uint16_t fragment_index = 0;

        // 完整 UDP datagram 大小
        std::size_t datagram_size = 0;

        // 完整 datagram 数据，包含：
        // MessageHeader + UdpFrameFragmentHeader + fragment payload
        std::vector<char> datagram;
    };

    // 缓存的一帧 UDP 分片集合。
    //
    // 一帧 H.264 数据可能被拆成多个 UDP 分片。为了支持 NACK，需要按帧缓存这些分片。
    struct CachedUdpFrame {
        // 帧序号，用于客户端请求重传时定位是哪一帧
        std::uint64_t frame_sequence = 0;

        // 该帧被缓存的时间，用于清理过期缓存
        std::uint64_t cached_timestamp_ns = 0;

        // 该帧包含的所有数据分片
        std::vector<CachedUdpFragment> fragments;
    };

    bool OpenSocket();
    void CloseSocket();

    // UDP 接收线程主循环。用于接收客户端 keepalive、receiver report 和 NACK 请求
    void ReceiveLoop();

    // 注册或更新客户端地址。
    //
    // 如果 address 已存在，则更新 last_seen_ns 和 report；
    // 如果 address 不存在，则新增一个客户端端点。
    void RegisterClient(
            const sockaddr_in &address,
            std::uint64_t now_ns,
            const common::net::UdpReceiverReport *report);

    // 清理超时客户端
    void PruneStaleClientsLocked(std::uint64_t now_ns);

    // 获取当前客户端地址快照
    std::vector<sockaddr_in> SnapshotClients(std::uint64_t now_ns);

    // 将一帧拆成多个 UDP 分片并发送给客户端
    bool SendFrameFragments(common::model::EncodedFramePtr frame, const std::vector<sockaddr_in> &clients);

    // 缓存已发送的一帧分片，用于后续 NACK 重传
    void CacheFrameFragments(const CachedUdpFrame &frame);

    // 清理过期的重传缓存。
    void PruneRetransmitCacheLocked(std::uint64_t now_ns);

    // 处理客户端 NACK 请求。客户端发现某些 UDP 分片丢失后，会请求服务端重传这些分片。
    void HandleNackRequest(
            const sockaddr_in &address,
            std::uint64_t now_ns,
            const common::net::UdpNackHeader &nack_header,
            const std::vector<common::net::UdpNackItem> &nack_items);

    // 格式化客户端地址。
    std::string FormatEndpoint(const sockaddr_in &address) const;

    // 格式化客户端接收统计报告
    std::string FormatClientReport(const UdpClientEndpoint &client) const;

    // 判断两个 UDP 端点是否相同， UDP 客户端由 IP + port 唯一标识。
    bool SameEndpoint(const sockaddr_in &lhs, const sockaddr_in &rhs) const;

private:
    // 传输配置。
    config::TransportConfig config_;

    // 后端名称，通常是 "udp"。
    std::string backend_name_;

    // 延迟日志输出间隔。
    int latency_log_interval_frames_;

    // UDP socket 文件描述符。
    int socket_fd_;

    // 实际绑定端口。
    int bound_port_;

    // 运行标志。
    std::atomic_bool running_;

    // UDP 接收线程。
    std::thread receive_thread_;

    // 客户端列表互斥锁。
    std::mutex clients_mutex_;

    // 当前已注册 UDP 客户端列表。
    std::vector<UdpClientEndpoint> clients_;

    // 重传缓存互斥锁。
    std::mutex retransmit_cache_mutex_;

    // NACK 重传缓存。
    std::deque<CachedUdpFrame> retransmit_cache_;

    // 发送延迟统计器。
    std::shared_ptr<common::metrics::LatencyRecorder> send_latency_recorder_;

    // 当前模块状态。
    std::atomic<core::ModuleState> state_;

    // 已成功发送的帧数量。
    std::uint64_t sent_frames_;

    // 因分片或发送失败而丢弃的帧数量。
    std::uint64_t dropped_fragmented_frames_;

    // 已发送 UDP 分片数量。
    std::uint64_t sent_fragments_;

    // 已发送 FEC 奇偶校验分片数量。
    std::uint64_t fec_fragments_sent_;

    // 发送失败的 UDP 分片数量。
    std::uint64_t failed_fragments_;

    // 收到的 NACK 请求数量。
    std::uint64_t nack_requests_received_;

    // 客户端请求重传的分片总数。
    std::uint64_t nack_fragments_requested_;

    // 实际成功重传的分片数量。
    std::uint64_t retransmitted_fragments_sent_;

    // NACK 请求命中失败次数。
    std::uint64_t retransmit_fragment_misses_;

    // 因重传限速而没有处理的分片数量。
    std::uint64_t retransmit_fragments_throttled_;

    // 发送 datagram 临时缓冲区。
    std::vector<char> send_datagram_;
};

}  // namespace udp
}  // namespace transport
}  // namespace modules
}  // namespace sserver

#endif  // SSERVER_MODULES_TRANSPORT_UDP_UDPSTREAMINGBACKEND_H
