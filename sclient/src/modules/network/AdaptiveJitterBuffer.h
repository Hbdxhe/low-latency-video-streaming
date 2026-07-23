#ifndef SCLIENT_MODULES_NETWORK_ADAPTIVEJITTERBUFFER_H
#define SCLIENT_MODULES_NETWORK_ADAPTIVEJITTERBUFFER_H

#include <algorithm>
#include <cstdint>
#include <string>

#include "common/log/Logger.h"
#include "common/metrics/LatencyStats.h"

namespace sclient {

/**
 * 抖动缓冲模式
 *
 * 根据网络质量自动切换，在延迟和流畅度之间取得平衡
 */
enum class JitterBufferMode {
    kBypass,      /**< 旁路模式：零延迟，适用于局域网 */
    kLowLatency,  /**< 低延迟模式：最小缓冲，适用于稳定网络 */
    kSmooth,      /**< 流畅模式：较大缓冲，适用于不稳定网络 */
};

/**
 * 网络质量评级
 *
 * 基于抖动、丢包率和跳帧率综合评估
 */
enum class NetworkQuality {
    kExcellent,  /**< 局域网/本地：旁路模式，最小缓冲 */
    kGood,       /**< 稳定 Wi-Fi：低延迟模式，适度缓冲 */
    kFair,       /**< 不稳定 Wi-Fi / 轻微丢包：流畅模式，较大缓冲 */
    kPoor,       /**< 高丢包/高抖动：流畅模式 + 积极保护 */
};

/** 自适应抖动缓冲配置 */
struct AdaptiveJitterBufferConfig {
    int min_delay_ms = 2;           /**< 最小延迟（毫秒） */
    double safety_factor = 1.5;     /**< 低延迟模式安全系数 */
    double smooth_factor = 2.5;     /**< 流畅模式安全系数 */
    int base_max_wait_ms = 80;      /**< 基础最大等待时间 */
    std::size_t base_max_frames = 8; /**< 基础最大缓冲帧数 */
};

/**
 * 自适应抖动缓冲器
 *
 * 根据网络质量自动调整缓冲策略：
 * - 网络良好时使用旁路模式，实现零延迟
 * - 网络一般时使用低延迟模式，最小化延迟
 * - 网络较差时使用流畅模式，优先保证播放流畅
 *
 * 状态机转换基于连续采样和 P95 抖动值，避免频繁切换
 */
class AdaptiveJitterBuffer {
public:
    AdaptiveJitterBuffer()
            : active_mode_(JitterBufferMode::kBypass),
              network_quality_(NetworkQuality::kExcellent),
              auto_mode_(true),
              window_(100, 4),
              consecutive_high_jitter_(0),
              consecutive_low_jitter_(0),
              low_to_smooth_p95_exceeded_(false),
              smooth_low_p95_since_ns_(0),
              last_logged_mode_(JitterBufferMode::kBypass),
              last_periodic_log_ns_(0),
              current_max_wait_ms_(80),
              current_max_frames_(8) {
    }

    /** 配置缓冲参数 */
    void Configure(const AdaptiveJitterBufferConfig &config) {
        config_ = config;
        current_max_wait_ms_ = config_.base_max_wait_ms;
        current_max_frames_ = config_.base_max_frames;
    }

    /** 记录抖动值（简化版本，无丢包和跳帧信息） */
    void RecordJitter(double jitter_ms, std::uint64_t now_ns) {
        RecordJitter(jitter_ms, 0.0, 0.0, now_ns);
    }

    /**
     * 记录抖动值并更新缓冲策略
     *
     * @param jitter_ms 当前抖动值（毫秒）
     * @param fragment_loss_percent 分片丢包率（百分比）
     * @param skip_rate_percent 跳帧率（百分比）
     * @param now_ns 当前时间戳（纳秒）
     */
    void RecordJitter(
            double jitter_ms,
            double fragment_loss_percent,
            double skip_rate_percent,
            std::uint64_t now_ns) {
        window_.Record(jitter_ms);

        AssessNetworkQuality(jitter_ms, fragment_loss_percent, skip_rate_percent);

        // 采样数足够后开始自动调整
        if (auto_mode_ && window_.sample_count() >= 10) {
            EvaluateStateTransition(jitter_ms, now_ns);
            ApplyQualityParams();
        }

        const double target_ms = static_cast<double>(TargetDelayNs()) / 1000000.0;

        // 模式或质量变化时记录日志
        if (active_mode_ != last_logged_mode_ || network_quality_ != last_logged_quality_) {
            common::log::Logger::Info(
                    "jitter buffer mode: " + ModeName(last_logged_mode_) + " -> " + ModeName(active_mode_)
                    + " quality=" + QualityName(network_quality_)
                    + ", target=" + FormatDouble(target_ms) + "ms"
                    + ", jitter_p95=" + FormatDouble(jitter_p95_ms()) + "ms"
                    + ", loss=" + FormatDouble(fragment_loss_percent) + "%");
            last_logged_mode_ = active_mode_;
            last_logged_quality_ = network_quality_;
        }

        // 每5秒记录一次状态日志
        if (now_ns - last_periodic_log_ns_ >= 5000000000ULL) {
            common::log::Logger::Info(
                    "jitter buffer status: mode=" + ModeName(active_mode_)
                    + " quality=" + QualityName(network_quality_)
                    + " target=" + FormatDouble(target_ms) + "ms"
                    + " p50=" + FormatDouble(jitter_p50_ms()) + "ms"
                    + " p95=" + FormatDouble(jitter_p95_ms()) + "ms"
                    + " loss=" + FormatDouble(fragment_loss_percent) + "%"
                    + " max_wait=" + std::to_string(current_max_wait_ms_) + "ms"
                    + " max_frames=" + std::to_string(current_max_frames_)
                    + " samples=" + std::to_string(window_.sample_count()));
            last_periodic_log_ns_ = now_ns;
        }
    }

    /** 获取目标延迟（纳秒） */
    std::uint64_t TargetDelayNs() const {
        return ComputeDelayForMode(active_mode_);
    }

    /** 设置固定模式（禁用自动调整） */
    void SetFixedMode(JitterBufferMode mode) {
        auto_mode_ = false;
        active_mode_ = mode;
    }

    /** 启用自动模式 */
    void EnableAutoMode() {
        auto_mode_ = true;
    }

    /** 重置所有状态 */
    void Reset() {
        window_ = LatencyStats(100, 4);
        active_mode_ = JitterBufferMode::kBypass;
        network_quality_ = NetworkQuality::kExcellent;
        consecutive_high_jitter_ = 0;
        consecutive_low_jitter_ = 0;
        low_to_smooth_p95_exceeded_ = false;
        smooth_low_p95_since_ns_ = 0;
        last_logged_mode_ = JitterBufferMode::kBypass;
        last_logged_quality_ = NetworkQuality::kExcellent;
        last_periodic_log_ns_ = 0;
        current_max_wait_ms_ = config_.base_max_wait_ms;
        current_max_frames_ = config_.base_max_frames;
    }

    JitterBufferMode active_mode() const { return active_mode_; }
    NetworkQuality network_quality() const { return network_quality_; }
    std::string active_mode_name() const { return ModeName(active_mode_); }
    std::string network_quality_name() const { return QualityName(network_quality_); }
    double jitter_p50_ms() const { return window_.Snapshot().p50_ms; }
    double jitter_p95_ms() const { return window_.Snapshot().p95_ms; }
    std::size_t jitter_samples() const { return window_.sample_count(); }
    int quality_max_wait_ms() const { return current_max_wait_ms_; }
    std::size_t quality_max_frames() const { return current_max_frames_; }

private:
    /** 评估网络质量等级 */
    void AssessNetworkQuality(double jitter_ms, double loss_percent, double skip_percent) {
        const double p95 = jitter_p95_ms();

        if (p95 > 50.0 || loss_percent > 1.0 || skip_percent > 5.0) {
            network_quality_ = NetworkQuality::kPoor;
        } else if (p95 > 10.0 || loss_percent > 0.1 || skip_percent > 1.0) {
            network_quality_ = NetworkQuality::kFair;
        } else if (p95 > 1.0 || loss_percent > 0.01) {
            network_quality_ = NetworkQuality::kGood;
        } else {
            network_quality_ = NetworkQuality::kExcellent;
        }
    }

    /** 根据网络质量调整缓冲参数 */
    void ApplyQualityParams() {
        switch (network_quality_) {
            case NetworkQuality::kExcellent:
                current_max_wait_ms_ = std::max(20, config_.base_max_wait_ms / 4);
                current_max_frames_ = std::max<std::size_t>(2, config_.base_max_frames / 4);
                break;
            case NetworkQuality::kGood:
                current_max_wait_ms_ = config_.base_max_wait_ms / 2;
                current_max_frames_ = std::max<std::size_t>(4, config_.base_max_frames / 2);
                break;
            case NetworkQuality::kFair:
                current_max_wait_ms_ = config_.base_max_wait_ms;
                current_max_frames_ = config_.base_max_frames;
                break;
            case NetworkQuality::kPoor:
                current_max_wait_ms_ = config_.base_max_wait_ms * 3;
                current_max_frames_ = config_.base_max_frames * 4;
                break;
        }
    }

    /**
     * 评估状态转换
     *
     * 状态机逻辑：
     * - Bypass -> LowLatency：连续10次抖动 > 1ms
     * - LowLatency -> Bypass：连续20次抖动 < 0.5ms
     * - LowLatency -> Smooth：P95 连续两次超过 10ms
     * - Smooth -> LowLatency：P95 持续低于 5ms 超过 3 秒
     */
    void EvaluateStateTransition(double jitter_ms, std::uint64_t now_ns) {
        const double p95 = jitter_p95_ms();

        switch (active_mode_) {
            case JitterBufferMode::kBypass:
                if (jitter_ms > 1.0) {
                    ++consecutive_high_jitter_;
                } else {
                    consecutive_high_jitter_ = 0;
                }
                if (consecutive_high_jitter_ >= 10) {
                    active_mode_ = JitterBufferMode::kLowLatency;
                    consecutive_high_jitter_ = 0;
                    consecutive_low_jitter_ = 0;
                    low_to_smooth_p95_exceeded_ = false;
                }
                break;

            case JitterBufferMode::kLowLatency:
                if (jitter_ms < 0.5) {
                    ++consecutive_low_jitter_;
                } else {
                    consecutive_low_jitter_ = 0;
                }
                if (consecutive_low_jitter_ >= 20) {
                    active_mode_ = JitterBufferMode::kBypass;
                    consecutive_low_jitter_ = 0;
                    consecutive_high_jitter_ = 0;
                    break;
                }
                if (p95 > 10.0) {
                    if (!low_to_smooth_p95_exceeded_) {
                        low_to_smooth_p95_exceeded_ = true;
                    } else {
                        active_mode_ = JitterBufferMode::kSmooth;
                        consecutive_low_jitter_ = 0;
                        consecutive_high_jitter_ = 0;
                        low_to_smooth_p95_exceeded_ = false;
                        smooth_low_p95_since_ns_ = 0;
                    }
                } else {
                    low_to_smooth_p95_exceeded_ = false;
                }
                break;

            case JitterBufferMode::kSmooth:
                if (p95 < 5.0) {
                    if (smooth_low_p95_since_ns_ == 0) {
                        smooth_low_p95_since_ns_ = now_ns;
                    } else if (now_ns - smooth_low_p95_since_ns_ >= 3000000000ULL) {
                        active_mode_ = JitterBufferMode::kLowLatency;
                        consecutive_low_jitter_ = 0;
                        consecutive_high_jitter_ = 0;
                        low_to_smooth_p95_exceeded_ = false;
                        smooth_low_p95_since_ns_ = 0;
                    }
                } else {
                    smooth_low_p95_since_ns_ = 0;
                }
                break;
        }
    }

    /** 根据当前模式计算目标延迟 */
    std::uint64_t ComputeDelayForMode(JitterBufferMode mode) const {
        switch (mode) {
            case JitterBufferMode::kBypass:
                return 0;

            case JitterBufferMode::kLowLatency: {
                const double p95 = jitter_p95_ms();
                const double target = std::max(
                        static_cast<double>(config_.min_delay_ms),
                        p95 * config_.safety_factor);
                return static_cast<std::uint64_t>(target * 1000000.0);
            }

            case JitterBufferMode::kSmooth: {
                const double p95 = jitter_p95_ms();
                const double target = std::max(
                        static_cast<double>(config_.min_delay_ms),
                        p95 * config_.smooth_factor);
                return static_cast<std::uint64_t>(target * 1000000.0);
            }
        }
        return 0;
    }

    static std::string ModeName(JitterBufferMode mode) {
        switch (mode) {
            case JitterBufferMode::kBypass: return "bypass";
            case JitterBufferMode::kLowLatency: return "low_latency";
            case JitterBufferMode::kSmooth: return "smooth";
        }
        return "unknown";
    }

    static std::string QualityName(NetworkQuality quality) {
        switch (quality) {
            case NetworkQuality::kExcellent: return "excellent";
            case NetworkQuality::kGood: return "good";
            case NetworkQuality::kFair: return "fair";
            case NetworkQuality::kPoor: return "poor";
        }
        return "unknown";
    }

    static std::string FormatDouble(double value) {
        char buf[32];
        std::snprintf(buf, sizeof(buf), "%.2f", value);
        return buf;
    }

    JitterBufferMode active_mode_;       /**< 当前激活的缓冲模式 */
    NetworkQuality network_quality_;     /**< 当前网络质量评级 */
    bool auto_mode_;                     /**< 是否启用自动模式 */
    LatencyStats window_;                /**< 抖动统计窗口（用于计算 P95） */

    int consecutive_high_jitter_;        /**< 连续高抖动计数 */
    int consecutive_low_jitter_;         /**< 连续低抖动计数 */
    bool low_to_smooth_p95_exceeded_;    /**< P95 是否已超过阈值（用于状态转换） */
    std::uint64_t smooth_low_p95_since_ns_; /**< P95 开始低于阈值的时间 */

    JitterBufferMode last_logged_mode_;  /**< 上次日志记录的模式 */
    NetworkQuality last_logged_quality_; /**< 上次日志记录的质量 */
    std::uint64_t last_periodic_log_ns_; /**< 上次周期性日志时间 */
    AdaptiveJitterBufferConfig config_;  /**< 配置参数 */

    int current_max_wait_ms_;            /**< 当前最大等待时间（根据质量调整） */
    std::size_t current_max_frames_;     /**< 当前最大缓冲帧数（根据质量调整） */
};

}  // namespace sclient

#endif  // SCLIENT_MODULES_NETWORK_ADAPTIVEJITTERBUFFER_H
