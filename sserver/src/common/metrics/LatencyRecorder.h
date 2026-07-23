#ifndef SSERVER_COMMON_METRICS_LATENCYRECORDER_H
#define SSERVER_COMMON_METRICS_LATENCYRECORDER_H

#include <cstddef>
#include <cstdint>
#include <deque>
#include <mutex>
#include <string>

namespace sserver {
namespace common {
namespace metrics {

// 一组延迟样本的统计摘要，所有延迟值的单位统一为毫秒。
//
// 为什么不仅记录平均值：
//   平均值容易掩盖少量但严重的延迟尖峰。例如大部分帧延迟为 10ms，
//   少数帧延迟为 200ms 时，平均值看起来可能仍然不高。因此还需要结合
//   p50、p95、p99 和最大值观察实时视频链路是否存在长尾抖动。
struct LatencySummary {
    // 本次统计快照包含的有效样本数量。
    // 当 count == 0 时，其余字段都保持默认值 0.0。
    std::size_t count = 0;

    // 最小延迟：当前窗口中最快的一次记录。
    double min_ms = 0.0;

    // 算术平均延迟：所有样本之和除以样本数量。
    double avg_ms = 0.0;

    // 第 50 百分位数，也称中位数。
    // 约有 50% 的样本不大于该值，适合表示一次“典型”的延迟体验。
    double p50_ms = 0.0;

    // 第 95 百分位数。
    // 约有 95% 的样本不大于该值，剩余约 5% 是较慢的长尾样本。
    double p95_ms = 0.0;

    // 第 99 百分位数。
    // 用来观察极少数严重卡顿，比平均值更容易暴露偶发延迟尖峰。
    double p99_ms = 0.0;

    // 最大延迟：当前窗口中最慢的一次记录。
    double max_ms = 0.0;
};

// 有界、线程安全的延迟样本记录器。
//
// 典型使用方式：
//   1. 工作线程计算两个单调时钟时间戳之差，得到纳秒延迟。
//   2. 调用 RecordNs() 将该延迟加入滑动窗口。
//   3. 日志线程、监控代码或测试调用 Snapshot()/Format() 查看统计结果。
//
// “有界”表示只保存最近 max_samples 个样本。窗口满后加入新样本时，
// 会淘汰最旧样本，从而避免服务器长期运行时内存持续增长。
//
// “线程安全”表示 RecordNs()、Snapshot() 和 Format() 可以由不同线程调用；
// samples_ms_ 的所有读写都由 mutex_ 保护。
class LatencyRecorder {
public:
    // 创建一个最多保存 max_samples 个延迟样本的记录器。
    //
    // 默认窗口为 4096 个样本。若视频帧率为 30 FPS，一个记录器大约保存
    // 最近 136 秒的帧延迟数据（4096 / 30）。
    //
    // 注意：当前实现要求 max_samples 必须大于 0。
    explicit LatencyRecorder(std::size_t max_samples = 4096);

    // 记录一个以纳秒为单位的延迟值。
    //
    // 接口使用纳秒，是因为链路计时通常来自高精度单调时钟；内部转换为
    // double 毫秒，便于后续统计和日志展示。例如：
    //   5,000,000ns -> 5.0ms
    //
    // 当窗口已满时，会先删除最旧样本，再保存当前最新样本。
    void RecordNs(std::uint64_t latency_ns);

    // 返回调用时刻的延迟统计快照。
    //
    // 该函数不会清空原始样本。实现会先在锁内复制样本，再在锁外完成
    // 排序和统计，避免耗时计算长时间阻塞正在调用 RecordNs() 的工作线程。
    LatencySummary Snapshot() const;

    // 将当前统计快照格式化为一行适合写入日志的文本。
    //
    // name 用于说明这组延迟属于哪个阶段，例如 capture_to_send。
    // 返回格式类似：
    //   capture_to_send count=120 min=5.00ms avg=8.20ms
    //   p50=7.80ms p95=12.10ms p99=15.30ms max=18.00ms
    std::string Format(const std::string &name) const;

private:
    // 滑动窗口允许保存的最大样本数量，构造后不再改变。
    const std::size_t max_samples_;

    // 保护 samples_ms_ 的互斥锁。
    //
    // Snapshot() 和 Format() 是 const 成员函数，但查询时仍然需要加锁。
    // 因此 mutex_ 使用 mutable：加锁属于同步操作，不改变记录器的业务数据。
    mutable std::mutex mutex_;

    // 按记录时间排列的延迟样本，单位为毫秒：
    //   队头 front() = 最旧样本
    //   队尾 back()  = 最新样本
    //
    // 使用 deque 是因为窗口满时需要高效地 pop_front() 淘汰最旧样本，
    // 同时需要 push_back() 追加最新样本。
    std::deque<double> samples_ms_;
};

}  // namespace metrics
}  // namespace common
}  // namespace sserver

#endif  // SSERVER_COMMON_METRICS_LATENCYRECORDER_H
