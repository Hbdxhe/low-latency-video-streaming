#ifndef SCLIENT_MODULES_NETWORK_TYPES_CLIENTCONFIG_H
#define SCLIENT_MODULES_NETWORK_TYPES_CLIENTCONFIG_H

#include <cstddef>
#include <string>

namespace sclient {

/**
 * 客户端配置
 *
 * 包含连接参数、传输协议配置和 UDP 优化参数
 */
struct ClientConfig {
    // 连接参数
    std::string host = "127.0.0.1";  /**< 服务器地址 */
    int port = 9999;                 /**< 服务器端口 */
    std::string transport = "tcp";   /**< 传输协议：tcp/udp/rtp */
    std::string sdp_path;            /**< SDP 文件路径（RTP 模式） */
    bool expect_metadata = true;     /**< 是否期望接收发送端元数据 */

    // UDP 传输参数
    std::size_t udp_max_datagram_size = 65507;    /**< UDP 数据报最大大小 */
    int udp_receive_buffer_bytes = 4 * 1024 * 1024; /**< UDP 接收缓冲区大小 */

    // RTP 参数
    int rtp_payload_type = -1;  /**< RTP 载荷类型（从 SDP 解析） */
    int rtp_clock_rate = 0;     /**< RTP 时钟频率（从 SDP 解析） */

    // UDP 抖动缓冲配置
    bool udp_jitter_buffer_enabled = true;               /**< 是否启用抖动缓冲 */
    std::string udp_jitter_buffer_strategy = "auto";     /**< 缓冲策略：auto/off/low/smooth/fixed/adaptive */
    int udp_jitter_buffer_target_delay_ms = 8;           /**< 目标延迟（毫秒） */
    int udp_jitter_buffer_adaptive_max_delay_ms = 120;   /**< 自适应模式最大延迟 */
    int udp_jitter_buffer_max_wait_ms = 40;              /**< 最大等待时间 */
    std::size_t udp_jitter_buffer_max_frames = 4;        /**< 缓冲区最大帧数 */
    int udp_jitter_buffer_min_delay_ms = 2;              /**< 最小延迟 */
    double udp_jitter_buffer_safety_factor = 1.5;        /**< 安全系数（用于计算缓冲延迟） */

    // UDP NACK 重传配置
    bool udp_nack_enabled = true;                    /**< 是否启用 NACK 重传 */
    bool udp_fec_enabled = true;                     /**< 是否启用 FEC 前向纠错 */
    int udp_nack_request_delay_ms = 25;              /**< NACK 请求延迟（等待乱序包） */
    int udp_nack_retry_interval_ms = 20;             /**< NACK 重试间隔 */
    int udp_nack_max_retries = 3;                    /**< 最大重试次数 */
    std::size_t udp_nack_max_requests_per_packet = 16; /**< 每个 NACK 包最大请求数 */

    // 测试用丢包/抖动注入（仅用于基准测试）
    std::string udp_test_jitter_pattern = "none";    /**< 抖动注入模式：none/saw/burst/alternate */
    int udp_test_jitter_amplitude_ms = 0;            /**< 抖动幅度（毫秒） */
    int udp_test_jitter_period = 6;                  /**< 抖动周期（帧数） */
    std::string udp_test_loss_pattern = "none";      /**< 丢包注入模式：none/single/burst/alternate */
    int udp_test_loss_period = 30;                   /**< 丢包周期（包数） */
    int udp_test_loss_count = 1;                     /**< 每次丢包数量 */

    int keepalive_interval_ms = 500;  /**< 心跳保活间隔（毫秒） */
};

}  // namespace sclient

#endif  // SCLIENT_MODULES_NETWORK_TYPES_CLIENTCONFIG_H
