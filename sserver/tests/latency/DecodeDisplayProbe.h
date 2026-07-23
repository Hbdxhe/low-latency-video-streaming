#ifndef SSERVER_TESTS_LATENCY_DECODEDISPLAYPROBE_H
#define SSERVER_TESTS_LATENCY_DECODEDISPLAYPROBE_H

#include <cstdint>
#include <string>

#include "common/metrics/LatencyRecorder.h"

namespace sserver {
namespace tests {
namespace latency {

class DecodeDisplayProbe {
public:
    DecodeDisplayProbe();
    ~DecodeDisplayProbe();

    bool Initialize(bool show_window, std::string *error_message);
    bool DecodeAndPresent(
            const std::uint8_t *data,
            std::size_t size,
            std::uint64_t capture_timestamp_ns,
            std::string *error_message);
    void Shutdown();

    const common::metrics::LatencyRecorder &decode_latency_recorder() const;
    const common::metrics::LatencyRecorder &present_latency_recorder() const;

private:
    struct Impl;
    Impl *impl_;
    common::metrics::LatencyRecorder decode_latency_recorder_;
    common::metrics::LatencyRecorder present_latency_recorder_;
};

}  // namespace latency
}  // namespace tests
}  // namespace sserver

#endif  // SSERVER_TESTS_LATENCY_DECODEDISPLAYPROBE_H
