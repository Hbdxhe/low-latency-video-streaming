#ifndef SCLIENT_MODULES_NETWORK_TYPES_UDPRECEIVESTATS_H
#define SCLIENT_MODULES_NETWORK_TYPES_UDPRECEIVESTATS_H

#include <cstddef>
#include <cstdint>
#include <string>

namespace sclient {

/**
 * UDP 接收统计信息
 *
 * 包含数据报接收、分片重组、抖动缓冲、NACK 重传和 FEC 恢复等统计数据
 */
struct UdpReceiveStats {
    // 数据报统计
    std::uint64_t datagrams_received = 0;      /**< 收到的 UDP 数据报总数 */
    std::uint64_t invalid_datagrams = 0;       /**< 无效数据报数（协议校验失败） */
    std::uint64_t stale_datagrams_dropped = 0; /**< 过期数据报数（属于已完成的帧） */

    // 分片统计
    std::uint64_t fragments_received = 0;      /**< 收到的分片总数 */
    std::uint64_t duplicate_fragments = 0;     /**< 重复分片数 */
    std::uint64_t timed_out_fragments = 0;     /**< 超时未到的分片数 */
    std::uint64_t timed_out_frames = 0;        /**< 超时未完整的帧数 */
    std::uint64_t completed_frames = 0;        /**< 完整接收的帧数 */
    std::uint64_t reordered_frames = 0;        /**< 经过重排序的帧数 */

    // 网络抖动统计
    double jitter_last_ms = 0.0;               /**< 最近一次抖动值（毫秒） */
    double jitter_avg_ms = 0.0;                /**< 平均抖动 */
    double jitter_max_ms = 0.0;                /**< 最大抖动 */
    double jitter_p50_ms = 0.0;                /**< 抖动中位数 */
    double jitter_p95_ms = 0.0;                /**< 抖动第95百分位 */
    std::uint64_t jitter_samples = 0;          /**< 抖动采样数 */

    // 抖动缓冲统计
    std::string jitter_buffer_active_mode = "bypass";  /**< 当前激活的缓冲模式 */
    std::string jitter_buffer_quality = "excellent";   /**< 缓冲质量评级 */
    std::size_t jitter_buffer_current_depth = 0;       /**< 当前缓冲区深度 */
    std::size_t jitter_buffer_max_depth = 0;           /**< 最大缓冲区深度 */
    std::uint64_t jitter_buffer_emitted_frames = 0;    /**< 正常输出的帧数 */
    std::uint64_t jitter_buffer_skipped_frames = 0;    /**< 跳过的帧数（追帧） */
    std::uint64_t jitter_buffer_dropped_frames = 0;    /**< 丢弃的帧数（缓冲区满） */
    std::uint64_t jitter_buffer_wait_samples = 0;      /**< 等待采样数 */
    double jitter_buffer_target_delay_ms = 0.0;        /**< 目标延迟（毫秒） */
    double jitter_buffer_wait_last_ms = 0.0;           /**< 最近一次等待时间 */
    double jitter_buffer_wait_avg_ms = 0.0;            /**< 平均等待时间 */
    double jitter_buffer_wait_max_ms = 0.0;            /**< 最大等待时间 */

    // NACK 重传统计
    std::uint64_t nack_requests_sent = 0;          /**< 发送的 NACK 请求数 */
    std::uint64_t nack_fragments_requested = 0;    /**< 请求重传的分片数 */

    // FEC 前向纠错统计
    std::uint64_t fec_fragments_received = 0;      /**< 收到的 FEC 分片数 */
    std::uint64_t fec_recovered_fragments = 0;     /**< FEC 恢复的分片数 */
    std::uint64_t fec_recovered_frames = 0;        /**< FEC 恢复的帧数 */

    // 测试注入统计（仅用于基准测试）
    std::uint64_t injected_loss_datagrams = 0;     /**< 注入丢包的数据报数 */
    std::uint64_t injected_loss_fragments = 0;     /**< 注入丢包的分片数 */
};

}  // namespace sclient

#endif  // SCLIENT_MODULES_NETWORK_TYPES_UDPRECEIVESTATS_H
