#include <cmath>
#include <iostream>
#include <string>

#include "common/metrics/LatencyRecorder.h"

namespace {

bool Expect(bool condition, const std::string &message) {
    if (!condition) {
        std::cerr << message << std::endl;
        return false;
    }
    return true;
}

bool ExpectNear(double actual, double expected, double tolerance, const std::string &message) {
    if (std::fabs(actual - expected) > tolerance) {
        std::cerr << message << " (expected ~" << expected << ", got " << actual << ")" << std::endl;
        return false;
    }
    return true;
}

bool TestEmptyRecorderReturnsZeroSummary() {
    sserver::common::metrics::LatencyRecorder recorder(100);
    const auto summary = recorder.Snapshot();
    return Expect(summary.count == 0, "expected count=0 for empty recorder");
}

bool TestSingleRecord() {
    sserver::common::metrics::LatencyRecorder recorder(100);
    recorder.RecordNs(1000000);  // 1 毫秒

    const auto summary = recorder.Snapshot();
    if (!Expect(summary.count == 1, "expected count=1")) return false;
    if (!ExpectNear(summary.min_ms, 1.0, 0.001, "expected min=1ms")) return false;
    if (!ExpectNear(summary.max_ms, 1.0, 0.001, "expected max=1ms")) return false;
    if (!ExpectNear(summary.avg_ms, 1.0, 0.001, "expected avg=1ms")) return false;
    return true;
}

bool TestMultipleRecords() {
    sserver::common::metrics::LatencyRecorder recorder(100);
    // 依次记录 1ms, 2ms, 3ms, 4ms, 5ms
    for (std::uint64_t i = 1; i <= 5; ++i) {
        recorder.RecordNs(i * 1000000);
    }

    const auto summary = recorder.Snapshot();
    if (!Expect(summary.count == 5, "expected count=5")) return false;
    if (!ExpectNear(summary.min_ms, 1.0, 0.001, "expected min=1ms")) return false;
    if (!ExpectNear(summary.max_ms, 5.0, 0.001, "expected max=5ms")) return false;
    if (!ExpectNear(summary.avg_ms, 3.0, 0.001, "expected avg=3ms")) return false;
    if (!ExpectNear(summary.p50_ms, 3.0, 0.001, "expected p50=3ms")) return false;
    return true;
}

bool TestCircularBufferEvictsOldest() {
    const std::size_t max_samples = 5;
    sserver::common::metrics::LatencyRecorder recorder(max_samples);

    // 向容量为 5 的缓冲区写入 10 个样本，验证最旧的样本被淘汰
    for (std::uint64_t i = 1; i <= 10; ++i) {
        recorder.RecordNs(i * 1000000);  // 1ms, 2ms, ..., 10ms
    }

    const auto summary = recorder.Snapshot();
    if (!Expect(summary.count == max_samples, "expected count to equal max_samples")) return false;
    if (!ExpectNear(summary.min_ms, 6.0, 0.001, "expected min=6ms (oldest 5 evicted)")) return false;
    if (!ExpectNear(summary.max_ms, 10.0, 0.001, "expected max=10ms")) return false;
    if (!ExpectNear(summary.avg_ms, 8.0, 0.001, "expected avg=8ms (6+7+8+9+10)/5")) return false;
    return true;
}

bool TestExactlyMaxSamples() {
    const std::size_t max_samples = 3;
    sserver::common::metrics::LatencyRecorder recorder(max_samples);

    recorder.RecordNs(1000000);  // 1ms
    recorder.RecordNs(2000000);  // 2ms
    recorder.RecordNs(3000000);  // 3ms

    const auto summary = recorder.Snapshot();
    if (!Expect(summary.count == 3, "expected count=3")) return false;
    if (!ExpectNear(summary.min_ms, 1.0, 0.001, "expected min=1ms")) return false;
    if (!ExpectNear(summary.max_ms, 3.0, 0.001, "expected max=3ms")) return false;

    // 再追加一个样本，验证最早写入的 1ms 被淘汰
    recorder.RecordNs(4000000);  // 4ms
    const auto summary2 = recorder.Snapshot();
    if (!Expect(summary2.count == 3, "expected count still=3")) return false;
    if (!ExpectNear(summary2.min_ms, 2.0, 0.001, "expected min=2ms after eviction")) return false;
    if (!ExpectNear(summary2.max_ms, 4.0, 0.001, "expected max=4ms after eviction")) return false;
    return true;
}

bool TestPercentileAccuracy() {
    sserver::common::metrics::LatencyRecorder recorder(1000);
    // 记录 100 个样本：1ms, 2ms, ..., 100ms
    for (std::uint64_t i = 1; i <= 100; ++i) {
        recorder.RecordNs(i * 1000000);
    }

    const auto summary = recorder.Snapshot();
    if (!Expect(summary.count == 100, "expected count=100")) return false;
    if (!ExpectNear(summary.p50_ms, 50.0, 1.0, "expected p50~50ms")) return false;
    if (!ExpectNear(summary.p95_ms, 95.0, 1.0, "expected p95~95ms")) return false;
    if (!ExpectNear(summary.p99_ms, 99.0, 1.0, "expected p99~99ms")) return false;
    return true;
}

bool TestFormatContainsName() {
    sserver::common::metrics::LatencyRecorder recorder(100);
    recorder.RecordNs(1000000);
    const std::string formatted = recorder.Format("test_metric");
    return Expect(formatted.find("test_metric") != std::string::npos,
                  "expected formatted string to contain metric name");
}

}  // namespace

int main() {
    if (!TestEmptyRecorderReturnsZeroSummary()) return EXIT_FAILURE;
    if (!TestSingleRecord()) return EXIT_FAILURE;
    if (!TestMultipleRecords()) return EXIT_FAILURE;
    if (!TestCircularBufferEvictsOldest()) return EXIT_FAILURE;
    if (!TestExactlyMaxSamples()) return EXIT_FAILURE;
    if (!TestPercentileAccuracy()) return EXIT_FAILURE;
    if (!TestFormatContainsName()) return EXIT_FAILURE;
    return EXIT_SUCCESS;
}
