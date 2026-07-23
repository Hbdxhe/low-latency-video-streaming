#ifndef SCLIENT_LATENCYSTATS_H
#define SCLIENT_LATENCYSTATS_H

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <iomanip>
#include <numeric>
#include <sstream>
#include <string>
#include <vector>

namespace sclient {

/**
 * 延迟统计摘要
 *
 * 包含样本数量、最小/最大/平均值和常用百分位数
 */
struct LatencySummary {
    std::size_t count = 0;    /**< 样本总数 */
    double last_ms = 0.0;     /**< 最近一次采样值（毫秒） */
    double min_ms = 0.0;      /**< 最小值 */
    double avg_ms = 0.0;      /**< 平均值 */
    double p50_ms = 0.0;      /**< 中位数（第50百分位） */
    double p95_ms = 0.0;      /**< 第95百分位 */
    double p99_ms = 0.0;      /**< 第99百分位 */
    double max_ms = 0.0;      /**< 最大值 */
};

/**
 * 延迟统计工具类
 *
 * 使用环形缓冲区存储最近N个采样值，支持计算各种统计指标。
 * 采用惰性刷新策略，避免每次查询都重新排序。
 *
 * 线程说明：本类不是线程安全的，调用方需要自行同步
 */
class LatencyStats {
public:
    /**
     * 构造函数
     *
     * @param max_samples 环形缓冲区大小，即最多保留的采样数
     * @param snapshot_refresh_interval 快照刷新间隔（采样次数）
     */
    explicit LatencyStats(std::size_t max_samples = 4096, std::size_t snapshot_refresh_interval = 8)
            : max_samples_(max_samples),
              snapshot_refresh_interval_(std::max<std::size_t>(1, snapshot_refresh_interval)),
              samples_ms_(max_samples, 0.0),
              next_index_(0),
              sample_count_(0),
              dirty_(false),
              cached_summary_valid_(false),
              pending_snapshot_samples_(0),
              last_ms_(0.0) {
    }

    /**
     * 记录一个采样值
     *
     * 环形缓冲区满时，最旧的采样值会被覆盖
     */
    void Record(double value_ms) {
        last_ms_ = value_ms;
        if (max_samples_ == 0) {
            dirty_ = true;
            cached_summary_valid_ = false;
            pending_snapshot_samples_ = 0;
            return;
        }

        samples_ms_[next_index_] = value_ms;
        next_index_ = (next_index_ + 1) % max_samples_;
        if (sample_count_ < max_samples_) {
            ++sample_count_;
        }

        dirty_ = true;
        ++pending_snapshot_samples_;
    }

    /** 是否有采样数据 */
    bool has_samples() const {
        return sample_count_ > 0;
    }

    /** 获取最近一次采样值 */
    double last_ms() const {
        return last_ms_;
    }

    /** 获取平均值 */
    double average_ms() const {
        return Snapshot().avg_ms;
    }

    /** 获取采样总数 */
    std::size_t sample_count() const {
        return sample_count_;
    }

    /**
     * 获取统计快照
     *
     * 返回包含各种统计指标的摘要对象
     */
    LatencySummary Snapshot() const {
        RefreshSummary(false);
        return cached_summary_;
    }

    /** 重置所有统计数据 */
    void Reset() {
        std::fill(samples_ms_.begin(), samples_ms_.end(), 0.0);
        next_index_ = 0;
        sample_count_ = 0;
        dirty_ = false;
        cached_summary_valid_ = false;
        pending_snapshot_samples_ = 0;
        last_ms_ = 0.0;
    }

    /**
     * 格式化统计信息为字符串
     *
     * 输出格式：name count=X min=Xms avg=Xms p50=Xms p95=Xms p99=Xms max=Xms last=Xms
     */
    std::string Format(const std::string &name) const {
        RefreshSummary(true);
        const LatencySummary summary = cached_summary_;
        std::ostringstream stream;
        stream << name
               << " count=" << summary.count
               << " min=" << std::fixed << std::setprecision(2) << summary.min_ms << "ms"
               << " avg=" << summary.avg_ms << "ms"
               << " p50=" << summary.p50_ms << "ms"
               << " p95=" << summary.p95_ms << "ms"
               << " p99=" << summary.p99_ms << "ms"
               << " max=" << summary.max_ms << "ms"
               << " last=" << summary.last_ms << "ms";
        return stream.str();
    }

private:
    /**
     * 刷新缓存的统计摘要
     *
     * 采用惰性刷新策略：
     * 1. 数据未变化且缓存有效时直接返回
     * 2. 非强制模式下，累积一定采样次数后才刷新（减少排序开销）
     * 3. 强制模式下立即刷新
     */
    void RefreshSummary(bool force_refresh) const {
        if (!dirty_ && cached_summary_valid_) {
            return;
        }

        if (!force_refresh &&
            cached_summary_valid_ &&
            pending_snapshot_samples_ < snapshot_refresh_interval_) {
            return;
        }

        cached_summary_ = BuildSummary();
        cached_summary_valid_ = true;
        dirty_ = false;
        pending_snapshot_samples_ = 0;
    }

    /**
     * 构建统计摘要
     *
     * 从环形缓冲区重建有序采样序列，然后计算各种统计指标。
     * 环形缓冲区可能已回绕，需要特殊处理顺序。
     */
    LatencySummary BuildSummary() const {
        LatencySummary summary;
        summary.count = sample_count_;
        summary.last_ms = last_ms_;
        if (sample_count_ == 0) {
            return summary;
        }

        // 从环形缓冲区重建按时间顺序的采样序列
        std::vector<double> ordered_samples(sample_count_);
        if (sample_count_ < max_samples_) {
            // 缓冲区未满，直接复制
            std::copy(samples_ms_.begin(), samples_ms_.begin() + static_cast<std::ptrdiff_t>(sample_count_), ordered_samples.begin());
        } else {
            // 缓冲区已回绕，需要分两段复制
            const std::size_t first_segment_size = max_samples_ - next_index_;
            std::copy(
                    samples_ms_.begin() + static_cast<std::ptrdiff_t>(next_index_),
                    samples_ms_.end(),
                    ordered_samples.begin());
            std::copy(
                    samples_ms_.begin(),
                    samples_ms_.begin() + static_cast<std::ptrdiff_t>(next_index_),
                    ordered_samples.begin() + static_cast<std::ptrdiff_t>(first_segment_size));
        }

        // 排序后计算百分位数
        std::vector<double> sorted_samples = ordered_samples;
        std::sort(sorted_samples.begin(), sorted_samples.end());
        summary.min_ms = sorted_samples.front();
        summary.max_ms = sorted_samples.back();
        summary.avg_ms = std::accumulate(ordered_samples.begin(), ordered_samples.end(), 0.0) /
                static_cast<double>(ordered_samples.size());
        summary.p50_ms = PercentileFromSorted(sorted_samples, 0.50);
        summary.p95_ms = PercentileFromSorted(sorted_samples, 0.95);
        summary.p99_ms = PercentileFromSorted(sorted_samples, 0.99);
        return summary;
    }

    /**
     * 从已排序数组计算百分位数
     *
     * 使用线性插值法，确保结果在[min, max]范围内
     */
    static double PercentileFromSorted(const std::vector<double> &values, double percentile) {
        if (values.empty()) {
            return 0.0;
        }

        const double index = percentile * static_cast<double>(values.size() - 1);
        const std::size_t lower = static_cast<std::size_t>(index);
        const std::size_t upper = std::min(lower + 1, values.size() - 1);
        const double fraction = index - static_cast<double>(lower);
        return values[lower] + (values[upper] - values[lower]) * fraction;
    }

private:
    std::size_t max_samples_;                  /**< 环形缓冲区最大容量 */
    std::size_t snapshot_refresh_interval_;    /**< 快照刷新间隔（采样次数） */
    std::vector<double> samples_ms_;           /**< 环形缓冲区 */
    std::size_t next_index_;                   /**< 下一个写入位置 */
    std::size_t sample_count_;                 /**< 已采样总数 */
    mutable bool dirty_;                       /**< 数据是否已变化 */
    mutable bool cached_summary_valid_;        /**< 缓存摘要是否有效 */
    mutable std::size_t pending_snapshot_samples_; /**< 自上次刷新以来的采样数 */
    mutable LatencySummary cached_summary_;    /**< 缓存的统计摘要 */
    double last_ms_;                           /**< 最近一次采样值 */
};

}  // namespace sclient

#endif  // SCLIENT_LATENCYSTATS_H
