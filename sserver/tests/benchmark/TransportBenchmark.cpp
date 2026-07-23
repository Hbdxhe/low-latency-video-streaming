#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include <chrono>
#include <cerrno>
#include <cstdlib>
#include <cstring>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

#include "app/AppBootstrap.h"
#include "common/log/Logger.h"
#include "common/metrics/LatencyRecorder.h"
#include "common/net/RtpProtocol.h"
#include "common/time/MonotonicClock.h"
#include "config/AppConfig.h"
#include "tests/support/TransportTestClient.h"

namespace {

struct BenchmarkOptions {
    std::string config_path = std::string(SSERVER_SOURCE_DIR) + "/config/benchmark_null_tcp.conf";
    int receive_delay_ms = 0;
};

struct ReceivedRtpPacket {
    sserver::common::net::RtpHeaderFields header;
    sserver::common::net::RtpHeaderExtension header_extension;
    sserver::common::net::RtpLatencyExtension latency_extension;
    std::uint64_t receive_timestamp_ns = 0;
};

bool ParsePositiveInt(const char *value, int *parsed) {
    if (value == nullptr || parsed == nullptr) {
        return false;
    }

    char *end = nullptr;
    const long parsed_value = std::strtol(value, &end, 10);
    if (end == value || *end != '\0' || parsed_value < 0 || parsed_value > 10000) {
        return false;
    }

    *parsed = static_cast<int>(parsed_value);
    return true;
}

bool ParseOptions(int argc, char **argv, BenchmarkOptions *options, std::string *error_message) {
    if (options == nullptr) {
        if (error_message != nullptr) {
            *error_message = "options output pointer is null";
        }
        return false;
    }

    BenchmarkOptions parsed_options;
    for (int index = 1; index < argc; ++index) {
        if (std::string(argv[index]) == "--config" && index + 1 < argc) {
            parsed_options.config_path = argv[index + 1];
            ++index;
        } else if (std::string(argv[index]) == "--receive-delay-ms" && index + 1 < argc) {
            if (!ParsePositiveInt(argv[index + 1], &parsed_options.receive_delay_ms)) {
                if (error_message != nullptr) {
                    *error_message = "invalid --receive-delay-ms value";
                }
                return false;
            }
            ++index;
        } else {
            if (error_message != nullptr) {
                *error_message = "unknown or incomplete argument: " + std::string(argv[index]);
            }
            return false;
        }
    }

    *options = parsed_options;
    return true;
}

bool OpenRtpReceiverSocket(const sserver::config::TransportConfig &transport_config, int *socket_fd) {
    if (socket_fd == nullptr) {
        return false;
    }

    *socket_fd = socket(AF_INET, SOCK_DGRAM, 0);
    if (*socket_fd < 0) {
        return false;
    }

    int reuse = 1;
    setsockopt(*socket_fd, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));
    setsockopt(
            *socket_fd,
            SOL_SOCKET,
            SO_RCVBUF,
            &transport_config.udp_receive_buffer_bytes,
            sizeof(transport_config.udp_receive_buffer_bytes));

    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_port = htons(static_cast<std::uint16_t>(transport_config.rtp_remote_port));
    if (inet_pton(AF_INET, transport_config.rtp_remote_host.c_str(), &address.sin_addr) != 1) {
        close(*socket_fd);
        *socket_fd = -1;
        return false;
    }

    if (bind(*socket_fd, reinterpret_cast<sockaddr *>(&address), sizeof(address)) != 0) {
        close(*socket_fd);
        *socket_fd = -1;
        return false;
    }

    timeval timeout{};
    timeout.tv_sec = 2;
    timeout.tv_usec = 0;
    setsockopt(*socket_fd, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));
    return true;
}

bool ReceiveRtpPacket(int socket_fd, ReceivedRtpPacket *packet, std::string *error_message) {
    if (packet == nullptr) {
        return false;
    }

    std::vector<std::uint8_t> datagram(65536, 0);
    const ssize_t received = recv(socket_fd, datagram.data(), datagram.size(), 0);
    if (received < 0) {
        if (error_message != nullptr) {
            *error_message = "failed to receive RTP datagram: " + std::string(std::strerror(errno));
        }
        return false;
    }
    if (received == 0) {
        if (error_message != nullptr) {
            *error_message = "RTP socket closed";
        }
        return false;
    }

    std::size_t header_size = 0;
    if (!sserver::common::net::ParseRtpHeader(
                datagram.data(),
                static_cast<std::size_t>(received),
                &packet->header,
                &header_size,
                &packet->header_extension)) {
        if (error_message != nullptr) {
            *error_message = "received invalid RTP header";
        }
        return false;
    }
    if (!sserver::common::net::ParseRtpLatencyExtension(packet->header_extension, &packet->latency_extension)) {
        if (error_message != nullptr) {
            *error_message = "received RTP packet without a valid latency header extension";
        }
        return false;
    }

    packet->receive_timestamp_ns = sserver::common::time::MonotonicNowNs();
    return true;
}

bool ReceiveRtpFrame(
        int socket_fd,
        std::uint8_t expected_payload_type,
        sserver::tests::support::ReceivedFrame *frame,
        std::string *error_message) {
    if (frame == nullptr) {
        return false;
    }

    bool have_timestamp = false;
    std::uint32_t expected_timestamp = 0;
    ReceivedRtpPacket first_packet;
    ReceivedRtpPacket last_packet;

    for (int index = 0; index < 256; ++index) {
        ReceivedRtpPacket packet;
        if (!ReceiveRtpPacket(socket_fd, &packet, error_message)) {
            return false;
        }
        if (packet.header.payload_type != expected_payload_type) {
            if (error_message != nullptr) {
                *error_message = "received unexpected RTP payload type";
            }
            return false;
        }

        if (!have_timestamp) {
            expected_timestamp = packet.header.timestamp;
            first_packet = packet;
            have_timestamp = true;
        } else if (packet.header.timestamp != expected_timestamp) {
            if (error_message != nullptr) {
                *error_message = "received mixed RTP timestamps before frame marker";
            }
            return false;
        }

        last_packet = packet;
        if (packet.header.marker) {
            std::memset(&frame->header, 0, sizeof(frame->header));
            std::memset(&frame->metadata, 0, sizeof(frame->metadata));
            frame->metadata.sequence = first_packet.header.timestamp;
            frame->metadata.capture_timestamp_ns = first_packet.latency_extension.capture_timestamp_ns;
            frame->metadata.transport_send_timestamp_ns = first_packet.latency_extension.transport_send_timestamp_ns;
            frame->receive_timestamp_ns = last_packet.receive_timestamp_ns;
            frame->payload.clear();
            return true;
        }
    }

    if (error_message != nullptr) {
        *error_message = "did not receive an RTP frame marker";
    }
    return false;
}

}  // namespace

int main(int argc, char **argv) {
    BenchmarkOptions options;
    std::string error_message;
    if (!ParseOptions(argc, argv, &options, &error_message)) {
        sserver::common::log::Logger::Error("failed to parse arguments: " + error_message);
        return 1;
    }

    sserver::config::AppConfig config;
    if (!sserver::config::ConfigLoader::LoadFromFile(options.config_path, &config, &error_message)) {
        sserver::common::log::Logger::Error("failed to load config: " + error_message);
        return 2;
    }
    const std::string log_file_path = sserver::common::log::Logger::CurrentLogFilePath();
    if (!log_file_path.empty()) {
        sserver::common::log::Logger::Info("log file: " + log_file_path);
    }
    if (config.transport.backend == "rtp") {
        if (!config.transport.rtp_enable_latency_extension) {
            sserver::common::log::Logger::Error(
                    "transport benchmark requires transport.rtp_enable_latency_extension=true for RTP");
            return 3;
        }
    } else if (!config.transport.embed_frame_metadata) {
        sserver::common::log::Logger::Error("transport benchmark requires transport.embed_frame_metadata=true");
        return 3;
    }

    {
        std::ostringstream stream;
        stream << "transport benchmark starting: config_path=" << options.config_path
               << ", backend=" << config.transport.backend
               << ", queue_drop_policy=" << config.transport.queue_drop_policy
               << ", max_pending_frames=" << config.transport.max_pending_frames
               << ", max_queue_wait_ms=" << config.transport.max_queue_wait_ms
               << ", receive_delay_ms=" << options.receive_delay_ms;
        sserver::common::log::Logger::Info(stream.str());
    }

    int socket_fd = -1;
    if (config.transport.backend == "rtp") {
        if (!OpenRtpReceiverSocket(config.transport, &socket_fd)) {
            sserver::common::log::Logger::Error("transport benchmark failed to open RTP receiver socket");
            return 6;
        }
    }

    sserver::app::AppBootstrap bootstrap(config);
    if (!bootstrap.Start()) {
        sserver::common::log::Logger::Error("failed to start application");
        if (socket_fd >= 0) {
            close(socket_fd);
        }
        return 4;
    }

    if (config.transport.backend != "rtp") {
        int port = 0;
        if (!sserver::tests::support::WaitForBoundPort(bootstrap, &port)) {
            sserver::common::log::Logger::Error("transport benchmark timed out waiting for bound port");
            bootstrap.Stop();
            return 5;
        }

        if (!sserver::tests::support::ConnectClient(config.transport, port, &socket_fd)) {
            sserver::common::log::Logger::Error("transport benchmark failed to connect client");
            bootstrap.Stop();
            return 6;
        }
    }

    sserver::common::metrics::LatencyRecorder receive_latency_recorder;
    sserver::common::metrics::LatencyRecorder capture_to_encode_start_recorder;
    sserver::common::metrics::LatencyRecorder encode_time_recorder;
    sserver::common::metrics::LatencyRecorder encode_to_send_recorder;
    sserver::common::metrics::LatencyRecorder send_to_receive_recorder;
    const int frames_to_measure = 120;
    for (int frame_index = 0; frame_index < frames_to_measure; ++frame_index) {
        if (!sserver::tests::support::RefreshClientKeepAlive(socket_fd, config.transport, frame_index)) {
            sserver::common::log::Logger::Error("transport benchmark failed to refresh keepalive");
            close(socket_fd);
            bootstrap.Stop();
            return 7;
        }

        sserver::tests::support::ReceivedFrame frame;
        bool receive_ok = false;
        if (config.transport.backend == "rtp") {
            receive_ok = ReceiveRtpFrame(
                    socket_fd,
                    static_cast<std::uint8_t>(config.transport.rtp_payload_type),
                    &frame,
                    &error_message);
        } else {
            receive_ok = sserver::tests::support::ReceiveFrame(socket_fd, config.transport, &frame);
        }
        if (!receive_ok) {
            sserver::common::log::Logger::Error("transport benchmark failed to receive frame");
            close(socket_fd);
            bootstrap.Stop();
            return 8;
        }

        if (frame.receive_timestamp_ns >= frame.metadata.capture_timestamp_ns &&
            frame.metadata.capture_timestamp_ns != 0) {
            receive_latency_recorder.RecordNs(frame.receive_timestamp_ns - frame.metadata.capture_timestamp_ns);
        }
        if (frame.metadata.capture_timestamp_ns != 0 &&
            frame.metadata.encode_start_timestamp_ns >= frame.metadata.capture_timestamp_ns) {
            capture_to_encode_start_recorder.RecordNs(
                    frame.metadata.encode_start_timestamp_ns - frame.metadata.capture_timestamp_ns);
        }
        if (frame.metadata.encode_end_timestamp_ns >= frame.metadata.encode_start_timestamp_ns &&
            frame.metadata.encode_start_timestamp_ns != 0) {
            encode_time_recorder.RecordNs(
                    frame.metadata.encode_end_timestamp_ns - frame.metadata.encode_start_timestamp_ns);
        }
        if (frame.metadata.transport_send_timestamp_ns >= frame.metadata.encode_end_timestamp_ns &&
            frame.metadata.encode_end_timestamp_ns != 0) {
            encode_to_send_recorder.RecordNs(
                    frame.metadata.transport_send_timestamp_ns - frame.metadata.encode_end_timestamp_ns);
        }
        if (frame.receive_timestamp_ns >= frame.metadata.transport_send_timestamp_ns &&
            frame.metadata.transport_send_timestamp_ns != 0) {
            send_to_receive_recorder.RecordNs(
                    frame.receive_timestamp_ns - frame.metadata.transport_send_timestamp_ns);
        }

        if (options.receive_delay_ms > 0) {
            std::this_thread::sleep_for(std::chrono::milliseconds(options.receive_delay_ms));
        }
    }

    close(socket_fd);

    sserver::common::log::Logger::Info(capture_to_encode_start_recorder.Format("capture_to_encode_start"));
    sserver::common::log::Logger::Info(encode_time_recorder.Format("encode_time"));
    sserver::common::log::Logger::Info(encode_to_send_recorder.Format("encode_to_send"));
    sserver::common::log::Logger::Info(bootstrap.send_latency_recorder()->Format("capture_to_send"));
    sserver::common::log::Logger::Info(send_to_receive_recorder.Format("network_to_receive"));
    sserver::common::log::Logger::Info(receive_latency_recorder.Format("capture_to_receive"));

    bootstrap.Stop();
    sserver::common::log::Logger::Info("transport benchmark completed successfully");
    return 0;
}
