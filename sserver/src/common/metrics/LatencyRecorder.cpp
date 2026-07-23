#include "common/metrics/LatencyRecorder.h"

#include <algorithm>
#include <iomanip>
#include <numeric>
#include <sstream>

namespace sserver {
namespace common {
namespace metrics {

namespace {

// 从已经按升序排列的样本中计算指定百分位数。
//
// Container 只需要支持 empty()、size() 和下标访问，因此该辅助函数可以
// 接受 deque、vector 等容器。它位于匿名命名空间中，只在本源文件内可见。
//
// percentile 使用 0.0 到 1.0 表示，例如：
//   0.50 -> p50
//   0.95 -> p95
//   0.99 -> p99
//
// 当百分位位置落在两个样本之间时，使用线性插值计算结果。
// 例如升序样本为 [10, 20, 30, 40]，计算 p50：
//   index    = 0.50 * (4 - 1) = 1.5
//   lower    = 1，对应 20
//   upper    = 2，对应 30
//   fraction = 0.5
//   result   = 20 + (30 - 20) * 0.5 = 25
template <typename Container>
double PercentileFromSorted(const Container &values, double percentile) {
    // 空集合没有可统计的百分位数，本项目约定返回 0.0。
    if (values.empty()) {
        return 0.0;
    }

    // 将 [0.0, 1.0] 范围的百分位映射到 [0, size - 1] 下标区间。
    //
    // 使用 size - 1 是为了保证：
    //   percentile == 0.0 时定位第一个样本；
    //   percentile == 1.0 时定位最后一个样本。
    const double index =
            percentile * static_cast<double>(values.size() - 1);

    // lower 是目标位置左侧的整数下标。double 转为 size_t 会截断小数，
    // 对非负 index 来说等价于向下取整。
    const std::size_t lower = static_cast<std::size_t>(index);

    // upper 通常是 lower 的下一个位置；当 lower 已是最后一个元素时，
    // 使用 min 将 upper 限制在合法下标范围内。
    const std::size_t upper =
            std::min(lower + 1, values.size() - 1);

    // fraction 表示目标位置从 lower 向 upper 前进的比例。
    // 若 index 恰好是整数，则 fraction == 0，结果就是 values[lower]。
    const double fraction = index - static_cast<double>(lower);

    // 在左右两个样本之间进行线性插值。
    return values[lower] +
           (values[upper] - values[lower]) * fraction;
}

}  // namespace

LatencyRecorder::LatencyRecorder(std::size_t max_samples)
        : max_samples_(max_samples) {
    // 这里只保存容量上限，不提前分配样本。
    // deque 会随着 RecordNs() 的调用按需分配存储空间。
}

void LatencyRecorder::RecordNs(std::uint64_t latency_ns) {
    // 纳秒适合高精度计时，毫秒更适合人类阅读和日志展示。
    //
    // 1ms = 1,000,000ns，因此除以 1,000,000.0。
    // 使用浮点除法可以保留亚毫秒精度，例如 500,000ns -> 0.5ms。
    const double latency_ms =
            static_cast<double>(latency_ns) / 1000000.0;

    // 从检查容量到删除旧样本、插入新样本的整个过程必须处于同一个
    // 临界区，否则多个记录线程可能同时判断“还有空间”并突破容量限制。
    std::lock_guard<std::mutex> lock(mutex_);

    // 窗口已满时淘汰队头的最旧样本，形成固定容量的滑动窗口。
    // 这样统计结果更能反映近期链路状态，也不会无限占用内存。
    if (samples_ms_.size() >= max_samples_) {
        samples_ms_.pop_front();
    }

    // 最新延迟始终追加到队尾。
    samples_ms_.push_back(latency_ms);
}

LatencySummary LatencyRecorder::Snapshot() const {
    std::deque<double> snapshot;

    {
        // 锁内只复制共享样本。离开大括号后 lock 自动析构并释放 mutex_。
        //
        // 排序的复杂度约为 O(n log n)，若在持锁状态下排序，会阻塞采集、
        // 编码或传输线程继续调用 RecordNs()，影响实时链路。
        std::lock_guard<std::mutex> lock(mutex_);
        snapshot = samples_ms_;
    }

    // summary 的所有字段都有默认值，因此空窗口可以直接返回一组零值。
    LatencySummary summary;
    summary.count = snapshot.size();
    if (snapshot.empty()) {
        return summary;
    }

    // 百分位数依赖样本的大小顺序，因此先将快照升序排列。
    // 排序的是副本，不会改变 samples_ms_ 原本的时间先后顺序。
    std::sort(snapshot.begin(), snapshot.end());

    // 升序排列后，首元素和尾元素分别是最小值和最大值。
    summary.min_ms = snapshot.front();
    summary.max_ms = snapshot.back();

    // std::accumulate(..., 0.0) 从 double 类型的 0.0 开始累加，
    // 得到所有延迟之和，再除以样本数量计算算术平均值。
    summary.avg_ms =
            std::accumulate(snapshot.begin(), snapshot.end(), 0.0) /
            static_cast<double>(snapshot.size());

    // 使用相同的线性插值算法计算常用百分位数。
    summary.p50_ms = PercentileFromSorted(snapshot, 0.50);
    summary.p95_ms = PercentileFromSorted(snapshot, 0.95);
    summary.p99_ms = PercentileFromSorted(snapshot, 0.99);

    return summary;
}

std::string LatencyRecorder::Format(const std::string &name) const {
    // Snapshot() 会在短暂加锁复制后完成统计，本函数只负责文本格式化。
    const LatencySummary summary = Snapshot();
    std::ostringstream stream;

    // fixed 表示使用普通小数形式，setprecision(2) 表示小数点后保留两位。
    // 这两个格式控制符会持续作用于后面写入 stream 的所有浮点数。
    stream << name
           << " count=" << summary.count
           << " min=" << std::fixed << std::setprecision(2)
           << summary.min_ms << "ms"
           << " avg=" << summary.avg_ms << "ms"
           << " p50=" << summary.p50_ms << "ms"
           << " p95=" << summary.p95_ms << "ms"
           << " p99=" << summary.p99_ms << "ms"
           << " max=" << summary.max_ms << "ms";

    return stream.str();
}

}  // namespace metrics
}  // namespace common
}  // namespace sserver
