#ifndef SCLIENT_VIDEORENDERER_H
#define SCLIENT_VIDEORENDERER_H

#include <cstdint>
#include <memory>
#include <string>

#include "common/media/DecodedFrame.h"
#include "common/metrics/LatencyStats.h"

namespace sclient {

/** 渲染后端类型 */
enum class RenderBackend {
    kAuto,    /**< 自动选择（当前仅支持 OpenGL） */
    kOpenGl,  /**< OpenGL 渲染 */
};

/**
 * 渲染帧信息
 *
 * 包含用于 HUD 显示的各种统计信息，每帧传递给渲染器
 */
struct RenderFrameInfo {
    std::string transport = "tcp";                  /**< 传输协议 */
    bool metadata_expected = false;                 /**< 是否期望发送端元数据 */
    bool sender_metadata_available = false;         /**< 发送端元数据是否可用 */

    // 延迟统计摘要（用于 HUD 显示）
    LatencySummary capture_to_render;               /**< 采集到渲染 */
    LatencySummary network_to_receive;              /**< 网络传输 */
    LatencySummary receive_to_decode;               /**< 接收到解码 */
    LatencySummary decode_time;                     /**< 解码耗时 */
    LatencySummary decode_to_render;                /**< 解码到渲染 */
    LatencySummary receive_to_render;               /**< 接收到渲染 */

    // UDP 抖动缓冲信息
    bool udp_jitter_buffer_enabled = false;         /**< 是否启用抖动缓冲 */
    std::string udp_jitter_buffer_strategy = "fixed"; /**< 缓冲策略 */
    std::string udp_jitter_buffer_active_mode;      /**< 当前激活模式 */
    std::string udp_jitter_buffer_quality;          /**< 缓冲质量 */
    double udp_jitter_p50_ms = 0.0;                 /**< 抖动中位数 */
    double udp_jitter_p95_ms = 0.0;                 /**< 抖动 P95 */
    std::size_t udp_jitter_buffer_current_depth = 0; /**< 当前缓冲深度 */
    std::size_t udp_jitter_buffer_max_depth = 0;    /**< 最大缓冲深度 */
    double udp_jitter_buffer_target_delay_ms = 0.0; /**< 目标延迟 */
    double udp_jitter_buffer_wait_avg_ms = 0.0;     /**< 平均等待时间 */
    double udp_jitter_buffer_wait_max_ms = 0.0;     /**< 最大等待时间 */
    std::uint64_t udp_jitter_buffer_skipped_frames = 0; /**< 跳过的帧数 */
    std::uint64_t udp_jitter_buffer_dropped_frames = 0; /**< 丢弃的帧数 */

    // 帧和队列信息
    int frame_width = 0;                            /**< 帧宽度 */
    int frame_height = 0;                           /**< 帧高度 */
    std::size_t receive_queue_depth = 0;            /**< 接收队列深度 */
    std::size_t decode_queue_depth = 0;             /**< 解码队列深度 */
    double fragment_loss_percent = 0.0;             /**< 分片丢包率 */
    bool connected = true;                          /**< 连接状态 */
    bool waiting_for_first_frame = true;            /**< 是否等待第一帧 */

    // NACK/FEC 信息
    bool udp_nack_enabled = false;                  /**< 是否启用 NACK */
    bool udp_fec_enabled = false;                   /**< 是否启用 FEC */
    std::uint64_t udp_nack_requests_sent = 0;       /**< 发送的 NACK 请求数 */
    std::uint64_t udp_fec_recovered_frames = 0;     /**< FEC 恢复的帧数 */
};

struct VideoRendererImpl;  /**< 渲染器实现（Pimpl 模式） */

/**
 * 视频渲染器
 *
 * 使用 OpenGL 渲染解码后的视频帧，支持 HUD 叠加显示统计信息
 */
class VideoRenderer {
public:
    VideoRenderer();
    ~VideoRenderer();

    /** 初始化渲染器 */
    bool Initialize(
            const std::string &window_title,
            RenderBackend requested_backend,
            bool enable_vsync,
            std::string *error_message,
            std::string *info_message);
    /** 检查渲染器是否支持该帧格式 */
    bool SupportsNativeFrame(const DecodedFrame &frame) const;
    /** 渲染一帧 */
    bool Render(const DecodedFrame &frame, const RenderFrameInfo &frame_info, std::string *error_message);
    /** 轮询键盘事件 */
    int PollKey(int delay_ms) const;
    /** 更新窗口标题 */
    void UpdateWindowTitle(const std::string &title);
    /** 切换全屏模式 */
    void ToggleFullscreen();
    /** 保存截图 */
    bool SaveScreenshot(const std::string &path, std::string *error_message);
    /** 关闭渲染器 */
    void Shutdown();

    RenderBackend backend() const;
    const std::string &backend_name() const;

private:
    std::unique_ptr<VideoRendererImpl> impl_;
};

}  // namespace sclient

#endif  // SCLIENT_VIDEORENDERER_H
