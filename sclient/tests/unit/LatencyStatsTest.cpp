#include <cmath>
#include <string>

#include "common/metrics/LatencyStats.h"
#include "tests/support/TestAssertions.h"

namespace {

using sclient::tests::support::Expect;
using sclient::tests::support::ExpectNear;

bool TestEmptyStats() {
    sclient::LatencyStats stats;

    if (!Expect(!stats.has_samples(), "empty stats should have no samples")) {
        return false;
    }
    if (!Expect(stats.sample_count() == 0, "expected sample_count 0")) {
        return false;
    }
    if (!Expect(stats.last_ms() == 0.0, "expected last_ms 0")) {
        return false;
    }

    const sclient::LatencySummary summary = stats.Snapshot();
    if (!Expect(summary.count == 0, "expected summary count 0")) {
        return false;
    }
    return true;
}

bool TestSingleSample() {
    sclient::LatencyStats stats;
    stats.Record(5.0);

    if (!Expect(stats.has_samples(), "should have samples")) {
        return false;
    }
    if (!Expect(stats.sample_count() == 1, "expected sample_count 1")) {
        return false;
    }
    if (!ExpectNear(stats.last_ms(), 5.0, 0.001, "expected last_ms 5.0")) {
        return false;
    }

    const sclient::LatencySummary summary = stats.Snapshot();
    if (!Expect(summary.count == 1, "expected summary count 1")) {
        return false;
    }
    if (!ExpectNear(summary.min_ms, 5.0, 0.001, "expected min 5.0")) {
        return false;
    }
    if (!ExpectNear(summary.max_ms, 5.0, 0.001, "expected max 5.0")) {
        return false;
    }
    if (!ExpectNear(summary.avg_ms, 5.0, 0.001, "expected avg 5.0")) {
        return false;
    }
    if (!ExpectNear(summary.p50_ms, 5.0, 0.001, "expected p50 5.0")) {
        return false;
    }
    return true;
}

bool TestMultipleSamples() {
    sclient::LatencyStats stats;
    for (int i = 1; i <= 10; ++i) {
        stats.Record(static_cast<double>(i));
    }

    if (!Expect(stats.sample_count() == 10, "expected 10 samples")) {
        return false;
    }
    if (!ExpectNear(stats.last_ms(), 10.0, 0.001, "expected last 10.0")) {
        return false;
    }

    const sclient::LatencySummary summary = stats.Snapshot();
    if (!ExpectNear(summary.min_ms, 1.0, 0.001, "expected min 1.0")) {
        return false;
    }
    if (!ExpectNear(summary.max_ms, 10.0, 0.001, "expected max 10.0")) {
        return false;
    }
    if (!ExpectNear(summary.avg_ms, 5.5, 0.001, "expected avg 5.5")) {
        return false;
    }
    if (!Expect(summary.p50_ms >= 4.0 && summary.p50_ms <= 7.0, "expected p50 around 5-6")) {
        return false;
    }
    return true;
}

bool TestCircularBufferOverflow() {
    sclient::LatencyStats stats(8, 1);

    for (int i = 1; i <= 20; ++i) {
        stats.Record(static_cast<double>(i));
    }

    if (!Expect(stats.sample_count() == 8, "expected sample_count capped at 8")) {
        return false;
    }
    if (!ExpectNear(stats.last_ms(), 20.0, 0.001, "expected last 20.0")) {
        return false;
    }

    const sclient::LatencySummary summary = stats.Snapshot();
    if (!Expect(summary.count == 8, "expected summary count 8")) {
        return false;
    }
    if (!ExpectNear(summary.min_ms, 13.0, 0.001, "expected min 13 (last 8 of 1..20)")) {
        return false;
    }
    if (!ExpectNear(summary.max_ms, 20.0, 0.001, "expected max 20")) {
        return false;
    }
    return true;
}

bool TestPercentileCalculation() {
    sclient::LatencyStats stats(100, 1);

    for (int i = 1; i <= 100; ++i) {
        stats.Record(static_cast<double>(i));
    }

    const sclient::LatencySummary summary = stats.Snapshot();
    if (!ExpectNear(summary.min_ms, 1.0, 0.001, "expected min 1")) {
        return false;
    }
    if (!ExpectNear(summary.max_ms, 100.0, 0.001, "expected max 100")) {
        return false;
    }
    if (!ExpectNear(summary.avg_ms, 50.5, 0.001, "expected avg 50.5")) {
        return false;
    }
    if (!Expect(summary.p50_ms >= 49.0 && summary.p50_ms <= 52.0, "expected p50 around 50")) {
        return false;
    }
    if (!Expect(summary.p95_ms >= 94.0 && summary.p95_ms <= 96.0, "expected p95 around 95")) {
        return false;
    }
    if (!Expect(summary.p99_ms >= 98.0 && summary.p99_ms <= 100.0, "expected p99 around 99")) {
        return false;
    }
    return true;
}

bool TestFormatOutput() {
    sclient::LatencyStats stats;
    stats.Record(1.5);
    stats.Record(2.5);

    const std::string formatted = stats.Format("test_metric");
    if (!Expect(formatted.find("count=2") != std::string::npos, "expected count=2 in output")) {
        return false;
    }
    if (!Expect(formatted.find("test_metric") != std::string::npos, "expected name in output")) {
        return false;
    }
    if (!Expect(formatted.find("min=") != std::string::npos, "expected min= in output")) {
        return false;
    }
    if (!Expect(formatted.find("avg=") != std::string::npos, "expected avg= in output")) {
        return false;
    }
    if (!Expect(formatted.find("p50=") != std::string::npos, "expected p50= in output")) {
        return false;
    }
    if (!Expect(formatted.find("last=") != std::string::npos, "expected last= in output")) {
        return false;
    }
    return true;
}

}  // namespace

int main() {
    if (!TestEmptyStats()) return 1;
    if (!TestSingleSample()) return 1;
    if (!TestMultipleSamples()) return 1;
    if (!TestCircularBufferOverflow()) return 1;
    if (!TestPercentileCalculation()) return 1;
    if (!TestFormatOutput()) return 1;
    return 0;
}
