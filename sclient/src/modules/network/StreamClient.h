#ifndef SCLIENT_STREAMCLIENT_H
#define SCLIENT_STREAMCLIENT_H

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <map>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_set>
#include <vector>

#include "common/protocol/Protocol.h"
#include "modules/network/AdaptiveJitterBuffer.h"
#include "modules/network/types/ClientConfig.h"
#include "modules/network/types/ReceivedFrame.h"
#include "modules/network/types/UdpReceiveStats.h"

namespace sclient {

/**
 * 流媒体客户端
 *
 * 支持 TCP、UDP、RTP 三种传输协议：
 * - TCP：简单可靠，按消息边界接收
 * - UDP：支持分片重组、NACK 重传、FEC 前向纠错和自适应抖动缓冲
 * - RTP：基于 UDP，支持 H.264 FU-A 分片重组
 */
class StreamClient {
public:
    StreamClient();
    ~StreamClient();

    /** 连接到服务器 */
    bool Connect(const ClientConfig &config, std::string *error_message);
    /** 接收一帧视频数据（阻塞） */
    bool ReceiveFrame(ReceivedFrame *frame, std::string *error_message);
    /** 获取 UDP 接收统计信息 */
    UdpReceiveStats udp_receive_stats() const;
    /** 获取绑定的本地端口 */
    int BoundPort() const;
    /** 关闭连接 */
    void Close();

private:
    /** RAII 守卫：在作用域结束时发布 UDP 统计快照 */
    class UdpStatsSnapshotGuard {
    public:
        explicit UdpStatsSnapshotGuard(StreamClient *client)
                : client_(client) {
        }

        ~UdpStatsSnapshotGuard() {
            if (client_ != nullptr) {
                client_->PublishUdpReceiveStatsSnapshot();
            }
        }

    private:
        StreamClient *client_;
    };

    /**
     * UDP 帧组装状态
     *
     * 大帧被分割成多个 UDP 分片传输，此结构跟踪组装进度
     */
    struct UdpFrameAssembly {
        protocol::MessageHeader header;
        protocol::FrameDiagnosticMetadata metadata;
        std::vector<std::uint8_t> payload;              /**< 组装后的帧数据 */
        std::vector<bool> received_fragments;           /**< 各分片是否已收到 */
        std::vector<std::uint32_t> fragment_offsets;     /**< 各分片在帧中的偏移 */
        std::vector<std::uint32_t> fragment_payload_sizes; /**< 各分片载荷大小 */
        std::size_t received_fragment_count = 0;        /**< 已收到的分片数 */
        std::vector<std::uint8_t> fec_payload;          /**< FEC 校验分片数据 */
        bool has_fec_payload = false;                   /**< 是否收到 FEC 分片 */
        std::uint64_t fec_payload_timestamp_ns = 0;     /**< FEC 分片时间戳 */
        std::uint64_t first_seen_timestamp_ns = 0;      /**< 首次见到此帧的时间 */
        std::uint64_t last_fragment_timestamp_ns = 0;   /**< 最后一个分片到达时间 */
        std::uint64_t last_nack_timestamp_ns = 0;       /**< 上次发送 NACK 的时间 */
        int nack_attempts = 0;                          /**< NACK 重试次数 */
    };

    /** 抖动缓冲区中的帧 */
    struct BufferedUdpFrame {
        ReceivedFrame frame;
        std::uint64_t buffered_timestamp_ns = 0;  /**< 入缓冲区时间 */
    };

    /**
     * RTP 帧组装状态
     *
     * RTP 包可能包含完整的 NAL 单元或 FU-A 分片，需要重组
     */
    struct RtpFrameAssembly {
        std::vector<std::uint8_t> payload;                /**< 组装后的帧数据 */
        std::uint32_t timestamp = 0;                      /**< RTP 时间戳（标识同一帧） */
        std::uint32_t ssrc = 0;                           /**< 同步源标识 */
        std::uint16_t next_sequence_number = 0;           /**< 期望的下一个序列号 */
        std::uint64_t capture_timestamp_ns = 0;           /**< 采集时间戳（从扩展解析） */
        std::uint64_t transport_send_timestamp_ns = 0;    /**< 发送时间戳（从扩展解析） */
        bool active = false;                              /**< 是否正在组装帧 */
        bool has_sequence_number = false;                 /**< 是否已设置序列号 */
        bool sender_metadata_available = false;           /**< 发送端元数据是否可用 */
        bool sender_metadata_invalid = false;             /**< 发送端元数据是否无效 */
        bool frame_damaged = false;                       /**< 帧是否损坏（丢包导致） */
        bool fu_in_progress = false;                      /**< 是否正在组装 FU-A 分片 */
    };

    // TCP 接收
    bool ReceiveAll(char *buffer, std::size_t length, std::string *error_message);
    bool ReceiveTcpFrame(ReceivedFrame *frame, std::string *error_message);

    // UDP 接收和处理
    bool ReceiveUdpFrame(ReceivedFrame *frame, std::string *error_message);
    bool ProcessUdpDatagram(
            const char *data,
            std::size_t datagram_size,
            std::uint64_t now_ns,
            bool defer_ready_pop,
            ReceivedFrame *frame,
            bool *frame_ready);
    bool FinalizeRecoverableUdpFecFrames(std::uint64_t now_ns, bool defer_ready_pop, ReceivedFrame *frame);
    bool TryPopReadyUdpFrame(ReceivedFrame *frame, std::uint64_t now_ns);
    bool FinalizeCompletedUdpFrame(
            std::uint64_t frame_sequence,
            UdpFrameAssembly *assembly,
            std::uint64_t now_ns,
            bool defer_ready_pop,
            ReceivedFrame *frame);

    // 抖动和丢包注入（测试用）
    std::uint64_t CurrentJitterBufferTargetDelayNs();
    std::uint64_t InjectedUdpJitterDelayNs(const ReceivedFrame &frame) const;
    bool ShouldInjectUdpLoss(const protocol::UdpFrameFragmentHeader &fragment_header);

    // FEC 前向纠错
    bool MaybeRecoverWithUdpFec(UdpFrameAssembly *assembly);

    // NACK 重传
    bool IsRecentlyCompletedUdpFrameSequence(std::uint64_t sequence) const;
    void RememberCompletedUdpFrameSequence(std::uint64_t sequence);
    void MaybeSendUdpNackRequests(std::uint64_t now_ns);
    bool SendUdpNack(std::uint64_t frame_sequence, const std::vector<std::uint16_t> &missing_fragments);

    // 状态管理
    void ResetUdpState();
    void ResetRtpState();
    void PruneExpiredUdpAssemblies(std::uint64_t now_ns);
    void BufferCompletedUdpFrame(ReceivedFrame &&frame);
    void OnCompletedUdpFrame(const ReceivedFrame &frame, std::uint64_t now_ns);

    // RTP 接收
    bool ReceiveRtpFrame(ReceivedFrame *frame, std::string *error_message);
    bool TryPopReadyRtpFrame(ReceivedFrame *frame);

    // 统计和保活
    void UpdateJitterBufferDepthStats();
    void RecordJitterBufferWait(std::uint64_t wait_ns);
    bool SendKeepAlive();
    void KeepAliveLoop();
    void PublishUdpReceiveStatsSnapshot();

private:
    ClientConfig config_;                           /**< 客户端配置 */
    int socket_fd_;                                 /**< socket 文件描述符 */
    std::atomic_bool running_;                      /**< 运行标志 */
    std::thread keepalive_thread_;                  /**< 心跳线程 */
    std::mutex send_mutex_;                         /**< 发送互斥锁 */
    mutable std::mutex udp_receive_stats_snapshot_mutex_; /**< 统计快照互斥锁 */
    UdpReceiveStats udp_receive_stats_;             /**< 实时 UDP 统计 */
    UdpReceiveStats udp_receive_stats_snapshot_;    /**< UDP 统计快照（供外部读取） */
    std::vector<char> udp_datagram_buffer_;         /**< UDP 接收缓冲区 */
    std::vector<char> rtp_datagram_buffer_;         /**< RTP 接收缓冲区 */
    std::map<std::uint64_t, UdpFrameAssembly> udp_assemblies_;  /**< UDP 帧组装表 */
    std::map<std::uint64_t, BufferedUdpFrame> udp_jitter_buffer_; /**< UDP 抖动缓冲区 */
    RtpFrameAssembly rtp_frame_assembly_;           /**< RTP 帧组装状态 */
    std::uint64_t last_completed_frame_sequence_;   /**< 上一个完成的帧序列号 */
    bool has_last_completed_frame_sequence_;
    std::uint64_t next_jitter_buffer_sequence_;     /**< 抖动缓冲区下一个期望序列号 */
    bool has_next_jitter_buffer_sequence_;
    std::map<std::uint64_t, std::vector<bool> > udp_test_dropped_fragments_; /**< 测试用丢包记录 */
    std::deque<std::uint64_t> recently_completed_udp_sequence_order_; /**< 最近完成的帧序列（用于去重） */
    std::unordered_set<std::uint64_t> recently_completed_udp_sequences_;
    double previous_network_latency_ms_;            /**< 上一次网络延迟（用于抖动计算） */
    bool has_previous_network_latency_;
    AdaptiveJitterBuffer adaptive_jitter_;          /**< 自适应抖动缓冲器 */
};

}  // namespace sclient

#endif  // SCLIENT_STREAMCLIENT_H
