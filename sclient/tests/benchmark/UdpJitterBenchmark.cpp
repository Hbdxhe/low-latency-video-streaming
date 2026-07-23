#include <cstdlib>
#include <cstring>
#include <sstream>
#include <string>

#include "app/cli/CliOptions.h"
#include "common/metrics/LatencyStats.h"
#include "common/log/Logger.h"
#include "modules/decoding/VideoDecoder.h"
#include "modules/network/StreamClient.h"
#include "tests/support/MonotonicClock.h"

namespace {

using sclient::common::log::Logger;
using sclient::tests::support::MonotonicNowNs;

void RecordLatencyBetween(std::uint64_t start_timestamp_ns, std::uint64_t end_timestamp_ns, sclient::LatencyStats *stats) {
    if (stats == nullptr || start_timestamp_ns == 0 || end_timestamp_ns < start_timestamp_ns) {
        return;
    }

    stats->Record(static_cast<double>(end_timestamp_ns - start_timestamp_ns) / 1000000.0);
}

void PrintUdpReceiveStats(const sclient::UdpReceiveStats &stats) {
    const std::uint64_t total_fragment_attempts = stats.fragments_received + stats.timed_out_fragments;
    const double fragment_loss_percent = total_fragment_attempts == 0
            ? 0.0
            : static_cast<double>(stats.timed_out_fragments) * 100.0 / static_cast<double>(total_fragment_attempts);

    std::ostringstream stream;
    stream << "udp_receive"
           << " datagrams=" << stats.datagrams_received
           << " invalid=" << stats.invalid_datagrams
           << " stale=" << stats.stale_datagrams_dropped
           << " fragments=" << stats.fragments_received
           << " duplicate=" << stats.duplicate_fragments
           << " timed_out_fragments=" << stats.timed_out_fragments
           << " timed_out_frames=" << stats.timed_out_frames
           << " completed_frames=" << stats.completed_frames
           << " reordered_frames=" << stats.reordered_frames
           << " fragment_loss=" << fragment_loss_percent << "%"
           << " jitter_last=" << stats.jitter_last_ms << "ms"
           << " jitter_avg=" << stats.jitter_avg_ms << "ms"
           << " jitter_max=" << stats.jitter_max_ms << "ms"
           << " jitter_buffer_depth=" << stats.jitter_buffer_current_depth
           << " jitter_buffer_max_depth=" << stats.jitter_buffer_max_depth
           << " jitter_buffer_target=" << stats.jitter_buffer_target_delay_ms << "ms"
           << " jitter_buffer_wait_avg=" << stats.jitter_buffer_wait_avg_ms << "ms"
           << " jitter_buffer_wait_max=" << stats.jitter_buffer_wait_max_ms << "ms"
           << " jitter_buffer_emitted=" << stats.jitter_buffer_emitted_frames
           << " jitter_buffer_skipped=" << stats.jitter_buffer_skipped_frames
           << " jitter_buffer_dropped=" << stats.jitter_buffer_dropped_frames
           << " nack_requests_sent=" << stats.nack_requests_sent
           << " nack_fragments_requested=" << stats.nack_fragments_requested
           << " fec_fragments_received=" << stats.fec_fragments_received
           << " fec_recovered_fragments=" << stats.fec_recovered_fragments
           << " fec_recovered_frames=" << stats.fec_recovered_frames
           << " injected_loss_datagrams=" << stats.injected_loss_datagrams
           << " injected_loss_fragments=" << stats.injected_loss_fragments;
    Logger::Info(stream.str());
}

}  // namespace

int main(int argc, char **argv) {
    sclient::ClientConfig config;
    config.transport = "udp";
    sclient::DecodeBackend decode_backend = sclient::DecodeBackend::kAuto;
    int frames_to_measure = 240;
    bool decode_frames = false;

    for (int index = 1; index < argc; ++index) {
        const sclient::CliParseResult parse_result = sclient::ParseUdpBenchmarkOption(
                argc,
                argv,
                &index,
                &config,
                &decode_backend,
                &frames_to_measure,
                &decode_frames);
        if (parse_result.show_help) {
            sclient::PrintUdpBenchmarkUsage(argv[0]);
            return 0;
        }
        if (!parse_result.success) {
            Logger::Error(parse_result.error_message);
            return 1;
        }
        if (!parse_result.handled) {
            sclient::PrintUdpBenchmarkUsage(argv[0]);
            return 1;
        }
    }

    sclient::StreamClient client;
    std::string error_message;
    if (!client.Connect(config, &error_message)) {
        Logger::Error("failed to connect stream client: " + error_message);
        return 2;
    }

    sclient::VideoDecoder decoder;
    if (decode_frames && !decoder.Initialize(decode_backend, &error_message)) {
        Logger::Error("failed to initialize decoder: " + error_message);
        return 3;
    }

    sclient::LatencyStats network_to_receive_stats;
    sclient::LatencyStats receive_to_release_stats;
    sclient::LatencyStats decode_time_stats;
    sclient::LatencyStats capture_to_release_stats;
    sclient::LatencyStats capture_to_decode_stats;
    sclient::LatencyStats release_gap_stats;
    sclient::DecodedFrame decoded_frame;
    std::uint64_t previous_release_timestamp_ns = 0;
    int released_frames = 0;
    int decoded_frames = 0;

    for (int frame_index = 0; frame_index < frames_to_measure; ++frame_index) {
        sclient::ReceivedFrame frame;
        if (!client.ReceiveFrame(&frame, &error_message)) {
            Logger::Error("stream receive failed: " + error_message);
            return 4;
        }

        const std::uint64_t release_timestamp_ns = MonotonicNowNs();
        ++released_frames;
        const std::uint64_t decode_start_timestamp_ns = release_timestamp_ns;
        std::uint64_t decode_end_timestamp_ns = decode_start_timestamp_ns;
        if (decode_frames) {
            if (!decoder.Decode(frame.payload.data(), frame.payload.size(), &decoded_frame, &error_message)) {
                continue;
            }
            decode_end_timestamp_ns = MonotonicNowNs();
            ++decoded_frames;
        }

        RecordLatencyBetween(frame.metadata.transport_send_timestamp_ns, frame.receive_timestamp_ns, &network_to_receive_stats);
        RecordLatencyBetween(frame.receive_timestamp_ns, release_timestamp_ns, &receive_to_release_stats);
        RecordLatencyBetween(frame.metadata.capture_timestamp_ns, release_timestamp_ns, &capture_to_release_stats);
        if (decode_frames) {
            RecordLatencyBetween(decode_start_timestamp_ns, decode_end_timestamp_ns, &decode_time_stats);
            RecordLatencyBetween(frame.metadata.capture_timestamp_ns, decode_end_timestamp_ns, &capture_to_decode_stats);
        }
        if (previous_release_timestamp_ns != 0) {
            RecordLatencyBetween(previous_release_timestamp_ns, release_timestamp_ns, &release_gap_stats);
        }
        previous_release_timestamp_ns = release_timestamp_ns;
    }

    const sclient::UdpReceiveStats udp_stats = client.udp_receive_stats();
    const std::uint64_t total_frame_outcomes = udp_stats.completed_frames + udp_stats.timed_out_frames;
    const double completion_rate = total_frame_outcomes == 0
            ? 0.0
            : static_cast<double>(udp_stats.completed_frames) * 100.0 / static_cast<double>(total_frame_outcomes);
    std::ostringstream benchmark_stream;
    benchmark_stream << "udp_jitter_benchmark"
                     << " host=" << config.host
                     << " port=" << config.port
                     << " frames=" << frames_to_measure
                     << " released=" << released_frames
                     << " decoded=" << decoded_frames
                     << " decode=" << (decode_frames ? "on" : "off")
                     << " jitter_buffer=" << (config.udp_jitter_buffer_enabled ? "on" : "off")
                     << " strategy=" << config.udp_jitter_buffer_strategy
                     << " target_ms=" << config.udp_jitter_buffer_target_delay_ms
                     << " adaptive_max_ms=" << config.udp_jitter_buffer_adaptive_max_delay_ms
                     << " max_wait_ms=" << config.udp_jitter_buffer_max_wait_ms
                     << " max_frames=" << config.udp_jitter_buffer_max_frames
                     << " nack=" << (config.udp_nack_enabled ? "on" : "off")
                     << " fec=" << (config.udp_fec_enabled ? "on" : "off")
                     << " nack_delay_ms=" << config.udp_nack_request_delay_ms
                     << " nack_retry_ms=" << config.udp_nack_retry_interval_ms
                     << " nack_max_retries=" << config.udp_nack_max_retries
                     << " inject_loss_pattern=" << config.udp_test_loss_pattern
                     << " inject_loss_period=" << config.udp_test_loss_period
                     << " inject_loss_count=" << config.udp_test_loss_count
                     << " inject_pattern=" << config.udp_test_jitter_pattern
                     << " inject_amplitude_ms=" << config.udp_test_jitter_amplitude_ms
                     << " inject_period=" << config.udp_test_jitter_period;
    Logger::Info(benchmark_stream.str());

    std::ostringstream recovery_stream;
    recovery_stream << "udp_recovery"
                    << " completion_rate=" << completion_rate << "%"
                    << " completed_frames=" << udp_stats.completed_frames
                    << " timed_out_frames=" << udp_stats.timed_out_frames
                    << " skipped_frames=" << udp_stats.jitter_buffer_skipped_frames
                    << " reordered_frames=" << udp_stats.reordered_frames
                    << " fec_recovered_frames=" << udp_stats.fec_recovered_frames
                    << " nack_requests_sent=" << udp_stats.nack_requests_sent
                    << " nack_fragments_requested=" << udp_stats.nack_fragments_requested
                    << " injected_loss_fragments=" << udp_stats.injected_loss_fragments;
    Logger::Info(recovery_stream.str());

    Logger::Info(network_to_receive_stats.Format("network_to_receive"));
    Logger::Info(receive_to_release_stats.Format("receive_to_release"));
    Logger::Info(capture_to_release_stats.Format("capture_to_release"));
    if (decode_frames) {
        Logger::Info(decode_time_stats.Format("decode_time"));
        Logger::Info(capture_to_decode_stats.Format("capture_to_decode"));
    }
    Logger::Info(release_gap_stats.Format("release_gap"));
    PrintUdpReceiveStats(udp_stats);
    return 0;
}
