#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <unistd.h>

#include <cerrno>
#include <chrono>
#include <cstring>
#include <map>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

#include "app/AppBootstrap.h"
#include "common/log/Logger.h"
#include "common/metrics/LatencyRecorder.h"
#include "common/net/H264AnnexB.h"
#include "common/net/RtpProtocol.h"
#include "common/net/StreamProtocol.h"
#include "common/time/MonotonicClock.h"
#include "config/AppConfig.h"
#include "DecodeDisplayProbe.h"

namespace {

struct BenchmarkFrame {
    sserver::common::net::MessageHeader header;
    sserver::common::net::FrameDiagnosticMetadata metadata;
    std::uint64_t receive_timestamp_ns = 0;
    std::vector<std::uint8_t> payload;
};

struct UdpFrameAssembly {
    sserver::common::net::MessageHeader header;
    sserver::common::net::FrameDiagnosticMetadata metadata;
    std::vector<std::uint8_t> payload;
    std::vector<bool> received_fragments;
    std::size_t received_fragment_count = 0;
};

struct ReceivedRtpPacket {
    sserver::common::net::RtpHeaderFields header;
    sserver::common::net::RtpHeaderExtension header_extension;
    sserver::common::net::RtpLatencyExtension latency_extension;
    std::uint64_t receive_timestamp_ns = 0;
    std::vector<std::uint8_t> payload;
};

bool ReceiveAll(int socket_fd, char *buffer, std::size_t length) {
    std::size_t received = 0;
    while (received < length) {
        const ssize_t result = recv(socket_fd, buffer + received, length - received, 0);
        if (result <= 0) {
            return false;
        }
        received += static_cast<std::size_t>(result);
    }
    return true;
}

std::string ParseConfigPath(int argc, char **argv) {
    std::string config_path = std::string(SSERVER_SOURCE_DIR) + "/config/benchmark.conf";
    for (int index = 1; index < argc; ++index) {
        if (std::strcmp(argv[index], "--config") == 0 && index + 1 < argc) {
            config_path = argv[index + 1];
            ++index;
        }
    }
    return config_path;
}

bool ParseShowWindow(int argc, char **argv) {
    for (int index = 1; index < argc; ++index) {
        if (std::strcmp(argv[index], "--show-window") == 0) {
            return true;
        }
    }
    return false;
}

bool IsV4L2DeviceAvailable(const std::string &device_path) {
    struct stat device_stat {};
    if (stat(device_path.c_str(), &device_stat) != 0) {
        return false;
    }
    return S_ISCHR(device_stat.st_mode);
}

bool SendKeepAlive(int socket_fd) {
    sserver::common::net::MessageHeader header{};
    sserver::common::net::FillMessageMagic(header.head_id);
    header.message_type = static_cast<std::uint16_t>(sserver::common::net::MessageType::kKeepAlive);
    header.payload_length = 0;
    return send(socket_fd, &header, sizeof(header), 0) == static_cast<ssize_t>(sizeof(header));
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

bool ReceiveTcpFrame(int socket_fd, bool expect_metadata, BenchmarkFrame *frame) {
    if (frame == nullptr) {
        return false;
    }

    if (!ReceiveAll(socket_fd, reinterpret_cast<char *>(&frame->header), sizeof(frame->header))) {
        return false;
    }
    if (!sserver::common::net::HasValidMessageMagic(frame->header)) {
        return false;
    }

    std::uint32_t payload_length = frame->header.payload_length;
    if (expect_metadata) {
        if (payload_length < sizeof(frame->metadata)) {
            return false;
        }
        if (!ReceiveAll(socket_fd, reinterpret_cast<char *>(&frame->metadata), sizeof(frame->metadata))) {
            return false;
        }
        payload_length -= sizeof(frame->metadata);
    } else {
        std::memset(&frame->metadata, 0, sizeof(frame->metadata));
    }

    frame->payload.resize(payload_length);
    if (!frame->payload.empty() &&
        !ReceiveAll(socket_fd, reinterpret_cast<char *>(frame->payload.data()), frame->payload.size())) {
        return false;
    }

    frame->receive_timestamp_ns = sserver::common::time::MonotonicNowNs();
    return true;
}

bool ReceiveUdpFrame(
        int socket_fd,
        std::size_t max_datagram_size,
        bool expect_metadata,
        BenchmarkFrame *frame) {
    if (frame == nullptr) {
        return false;
    }

    std::map<std::uint64_t, UdpFrameAssembly> assemblies;

    while (true) {
        std::vector<char> datagram(max_datagram_size);
        const ssize_t received = recv(socket_fd, datagram.data(), datagram.size(), 0);
        if (received <= 0) {
            return false;
        }

        const std::size_t datagram_size = static_cast<std::size_t>(received);
        if (datagram_size < sizeof(frame->header) + sizeof(sserver::common::net::UdpFrameFragmentHeader)) {
            continue;
        }

        sserver::common::net::MessageHeader header{};
        std::memcpy(&header, datagram.data(), sizeof(header));
        if (!sserver::common::net::HasValidMessageMagic(header)) {
            continue;
        }
        if (header.message_type != static_cast<std::uint16_t>(sserver::common::net::MessageType::kAvStream)) {
            continue;
        }
        if (header.payload_length < sizeof(sserver::common::net::UdpFrameFragmentHeader)) {
            continue;
        }

        sserver::common::net::UdpFrameFragmentHeader fragment_header{};
        std::memcpy(&fragment_header, datagram.data() + sizeof(header), sizeof(fragment_header));
        if (fragment_header.fragment_count == 0 || fragment_header.fragment_index >= fragment_header.fragment_count) {
            continue;
        }

        const std::size_t fragment_payload_size = datagram_size - sizeof(header) - sizeof(fragment_header);
        if (fragment_payload_size + sizeof(fragment_header) != header.payload_length) {
            continue;
        }
        if (fragment_header.fragment_offset + fragment_payload_size > fragment_header.frame_payload_size) {
            continue;
        }

        UdpFrameAssembly &assembly = assemblies[fragment_header.frame_sequence];
        if (assembly.received_fragments.empty()) {
            assembly.header = header;
            assembly.metadata.sequence = fragment_header.frame_sequence;
            assembly.metadata.capture_timestamp_ns = expect_metadata ? fragment_header.capture_timestamp_ns : 0;
            assembly.metadata.encode_start_timestamp_ns = expect_metadata ? fragment_header.encode_start_timestamp_ns : 0;
            assembly.metadata.encode_end_timestamp_ns = expect_metadata ? fragment_header.encode_end_timestamp_ns : 0;
            assembly.metadata.transport_send_timestamp_ns = expect_metadata ? fragment_header.transport_send_timestamp_ns : 0;
            assembly.payload.resize(fragment_header.frame_payload_size);
            assembly.received_fragments.assign(fragment_header.fragment_count, false);
        }

        if (assembly.received_fragments.size() != fragment_header.fragment_count ||
            assembly.payload.size() != fragment_header.frame_payload_size) {
            assemblies.erase(fragment_header.frame_sequence);
            continue;
        }

        if (!assembly.received_fragments[fragment_header.fragment_index]) {
            std::memcpy(
                    assembly.payload.data() + fragment_header.fragment_offset,
                    datagram.data() + sizeof(header) + sizeof(fragment_header),
                    fragment_payload_size);
            assembly.received_fragments[fragment_header.fragment_index] = true;
            ++assembly.received_fragment_count;
        }

        if (assembly.received_fragment_count == assembly.received_fragments.size()) {
            frame->header = assembly.header;
            frame->metadata = assembly.metadata;
            frame->receive_timestamp_ns = sserver::common::time::MonotonicNowNs();
            frame->payload.swap(assembly.payload);
            assemblies.erase(fragment_header.frame_sequence);
            return true;
        }
    }
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
    packet->payload.assign(
            datagram.begin() + static_cast<std::vector<std::uint8_t>::difference_type>(header_size),
            datagram.begin() + static_cast<std::vector<std::uint8_t>::difference_type>(received));
    return true;
}

bool AppendRtpPayloadAsAnnexB(
        const ReceivedRtpPacket &packet,
        std::vector<std::uint8_t> *annexb_payload,
        bool *fragment_open,
        std::string *error_message) {
    if (annexb_payload == nullptr || fragment_open == nullptr || packet.payload.empty()) {
        if (error_message != nullptr) {
            *error_message = "rtp packet payload is empty";
        }
        return false;
    }

    const std::uint8_t nal_type = static_cast<std::uint8_t>(packet.payload[0] & 0x1F);
    if (nal_type > 0 && nal_type < 24) {
        if (*fragment_open) {
            if (error_message != nullptr) {
                *error_message = "received single NAL while FU-A reassembly is open";
            }
            return false;
        }
        sserver::common::net::AppendAnnexBNalu(packet.payload.data(), packet.payload.size(), annexb_payload);
        return true;
    }

    if (nal_type != sserver::common::net::kH264FuAType || packet.payload.size() < 2) {
        if (error_message != nullptr) {
            *error_message = "unsupported RTP/H264 payload format";
        }
        return false;
    }

    const std::uint8_t fu_indicator = packet.payload[0];
    const std::uint8_t fu_header = packet.payload[1];
    const bool start = (fu_header & 0x80) != 0;
    const bool end = (fu_header & 0x40) != 0;
    const std::uint8_t reconstructed_nalu_header =
            static_cast<std::uint8_t>((fu_indicator & 0xE0) | (fu_header & 0x1F));

    if (start) {
        if (*fragment_open) {
            if (error_message != nullptr) {
                *error_message = "received nested FU-A start fragment";
            }
            return false;
        }
        sserver::common::net::AppendAnnexBStartCode(annexb_payload);
        annexb_payload->push_back(reconstructed_nalu_header);
        annexb_payload->insert(annexb_payload->end(), packet.payload.begin() + 2, packet.payload.end());
        *fragment_open = !end;
        return true;
    }

    if (!*fragment_open) {
        if (error_message != nullptr) {
            *error_message = "received FU-A continuation without start fragment";
        }
        return false;
    }

    annexb_payload->insert(annexb_payload->end(), packet.payload.begin() + 2, packet.payload.end());
    if (end) {
        *fragment_open = false;
    }
    return true;
}

bool ReceiveRtpFrame(
        int socket_fd,
        std::uint8_t expected_payload_type,
        BenchmarkFrame *frame,
        std::string *error_message) {
    if (frame == nullptr) {
        return false;
    }

    frame->payload.clear();
    bool have_timestamp = false;
    std::uint32_t expected_timestamp = 0;
    bool fragment_open = false;
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

        if (!AppendRtpPayloadAsAnnexB(packet, &frame->payload, &fragment_open, error_message)) {
            return false;
        }
        last_packet = packet;
        if (packet.header.marker) {
            if (fragment_open) {
                if (error_message != nullptr) {
                    *error_message = "RTP frame ended while FU-A reassembly was still open";
                }
                return false;
            }

            std::memset(&frame->header, 0, sizeof(frame->header));
            std::memset(&frame->metadata, 0, sizeof(frame->metadata));
            frame->metadata.sequence = first_packet.header.timestamp;
            frame->metadata.capture_timestamp_ns = first_packet.latency_extension.capture_timestamp_ns;
            frame->metadata.transport_send_timestamp_ns = first_packet.latency_extension.transport_send_timestamp_ns;
            frame->receive_timestamp_ns = last_packet.receive_timestamp_ns;
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
    const std::string config_path = ParseConfigPath(argc, argv);
    const bool show_window = ParseShowWindow(argc, argv);

    sserver::config::AppConfig config;
    std::string error_message;
    if (!sserver::config::ConfigLoader::LoadFromFile(config_path, &config, &error_message)) {
        sserver::common::log::Logger::Error("failed to load config: " + error_message);
        return 1;
    }
    const std::string log_file_path = sserver::common::log::Logger::CurrentLogFilePath();
    if (!log_file_path.empty()) {
        sserver::common::log::Logger::Info("log file: " + log_file_path);
    }
    if (config.capture.source == "v4l2" && !IsV4L2DeviceAvailable(config.capture.device)) {
        sserver::common::log::Logger::Warn("v4l2 capture device is unavailable, skipping latency benchmark");
        return 77;
    }
    if (config.transport.backend == "rtp") {
        if (!config.transport.rtp_enable_latency_extension) {
            sserver::common::log::Logger::Error(
                    "latency benchmark requires transport.rtp_enable_latency_extension=true for RTP");
            return 12;
        }
    } else if (!config.transport.embed_frame_metadata) {
        sserver::common::log::Logger::Error("latency benchmark requires transport.embed_frame_metadata=true");
        return 12;
    }
    {
        std::ostringstream stream;
        stream << "latency benchmark starting: config_path=" << config_path
               << ", backend=" << config.transport.backend
               << ", show_window=" << (show_window ? "true" : "false")
               << ", udp_max_datagram_size=" << config.transport.udp_max_datagram_size;
        sserver::common::log::Logger::Info(stream.str());
    }

    int socket_fd = -1;
    if (config.transport.backend == "rtp") {
        if (!OpenRtpReceiverSocket(config.transport, &socket_fd)) {
            sserver::common::log::Logger::Error("latency benchmark failed to open RTP receiver socket");
            return 4;
        }
    }

    sserver::app::AppBootstrap bootstrap(config);
    if (!bootstrap.Start()) {
        sserver::common::log::Logger::Error("failed to start application");
        if (socket_fd >= 0) {
            close(socket_fd);
        }
        return 2;
    }

    if (config.transport.backend != "rtp") {
        int port = 0;
        for (int attempt = 0; attempt < 50 && port == 0; ++attempt) {
            port = bootstrap.bound_port();
            if (port == 0) {
                std::this_thread::sleep_for(std::chrono::milliseconds(20));
            }
        }
        if (port == 0) {
            sserver::common::log::Logger::Error("latency benchmark timed out waiting for bound port");
            bootstrap.Stop();
            return 3;
        }

        const int socket_type = config.transport.backend == "udp" ? SOCK_DGRAM : SOCK_STREAM;
        socket_fd = socket(AF_INET, socket_type, 0);
        if (socket_fd < 0) {
            sserver::common::log::Logger::Error("latency benchmark failed to create socket");
            bootstrap.Stop();
            return 4;
        }

        sockaddr_in address{};
        address.sin_family = AF_INET;
        address.sin_port = htons(static_cast<std::uint16_t>(port));
        address.sin_addr.s_addr = inet_addr(config.transport.bind_address.c_str());

        if (connect(socket_fd, reinterpret_cast<sockaddr *>(&address), sizeof(address)) != 0) {
            sserver::common::log::Logger::Error("latency benchmark failed to connect to transport backend");
            close(socket_fd);
            bootstrap.Stop();
            return 5;
        }

        if (config.transport.backend == "udp") {
            setsockopt(
                    socket_fd,
                    SOL_SOCKET,
                    SO_RCVBUF,
                    &config.transport.udp_receive_buffer_bytes,
                    sizeof(config.transport.udp_receive_buffer_bytes));
        }
        if (!SendKeepAlive(socket_fd)) {
            sserver::common::log::Logger::Error("latency benchmark failed to send initial keepalive");
            close(socket_fd);
            bootstrap.Stop();
            return 13;
        }
    }

    sserver::common::metrics::LatencyRecorder receive_recorder;
    sserver::common::metrics::LatencyRecorder capture_to_encode_start_recorder;
    sserver::common::metrics::LatencyRecorder encode_time_recorder;
    sserver::common::metrics::LatencyRecorder encode_to_send_recorder;
    sserver::common::metrics::LatencyRecorder send_to_receive_recorder;
    sserver::tests::latency::DecodeDisplayProbe decode_display_probe;
    if (!decode_display_probe.Initialize(show_window, &error_message)) {
        sserver::common::log::Logger::Error("failed to initialize decode/display probe: " + error_message);
        close(socket_fd);
        bootstrap.Stop();
        return 10;
    }
    const int frames_to_measure = 120;
    int decoded_frames = 0;
    int receive_attempts = 0;
    const int max_receive_attempts = frames_to_measure + 180;

    while (decoded_frames < frames_to_measure && receive_attempts < max_receive_attempts) {
        if (config.transport.backend != "rtp" &&
            receive_attempts % 15 == 0 &&
            !SendKeepAlive(socket_fd)) {
            sserver::common::log::Logger::Error("latency benchmark failed to refresh keepalive");
            close(socket_fd);
            decode_display_probe.Shutdown();
            bootstrap.Stop();
            return 14;
        }

        BenchmarkFrame frame;
        bool received_frame = false;
        if (config.transport.backend == "udp") {
            received_frame = ReceiveUdpFrame(
                    socket_fd,
                    config.transport.udp_max_datagram_size,
                    config.transport.embed_frame_metadata,
                    &frame);
        } else if (config.transport.backend == "rtp") {
            received_frame = ReceiveRtpFrame(
                    socket_fd,
                    static_cast<std::uint8_t>(config.transport.rtp_payload_type),
                    &frame,
                    &error_message);
        } else {
            received_frame = ReceiveTcpFrame(socket_fd, config.transport.embed_frame_metadata, &frame);
        }
        ++receive_attempts;
        if (!received_frame) {
            sserver::common::log::Logger::Error("latency benchmark failed to receive frame");
            close(socket_fd);
            decode_display_probe.Shutdown();
            bootstrap.Stop();
            return 6;
        }

        if (frame.receive_timestamp_ns >= frame.metadata.capture_timestamp_ns &&
            frame.metadata.capture_timestamp_ns != 0) {
            receive_recorder.RecordNs(frame.receive_timestamp_ns - frame.metadata.capture_timestamp_ns);
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

        error_message.clear();
        if (!decode_display_probe.DecodeAndPresent(
                    frame.payload.empty() ? nullptr : frame.payload.data(),
                    frame.payload.size(),
                    frame.metadata.capture_timestamp_ns,
                    &error_message)) {
            if (config.transport.backend == "rtp" && error_message == "decoder did not output a frame for the packet") {
                continue;
            }
            sserver::common::log::Logger::Error("failed to decode or present frame: " + error_message);
            close(socket_fd);
            decode_display_probe.Shutdown();
            bootstrap.Stop();
            return 11;
        }
        ++decoded_frames;
    }

    if (decoded_frames < frames_to_measure) {
        sserver::common::log::Logger::Error("latency benchmark did not decode enough frames");
        close(socket_fd);
        decode_display_probe.Shutdown();
        bootstrap.Stop();
        return 15;
    }

    close(socket_fd);

    sserver::common::log::Logger::Info(capture_to_encode_start_recorder.Format("capture_to_encode_start"));
    sserver::common::log::Logger::Info(encode_time_recorder.Format("encode_time"));
    sserver::common::log::Logger::Info(encode_to_send_recorder.Format("encode_to_send"));
    sserver::common::log::Logger::Info(bootstrap.send_latency_recorder()->Format("capture_to_send"));
    sserver::common::log::Logger::Info(send_to_receive_recorder.Format("network_to_receive"));
    sserver::common::log::Logger::Info(receive_recorder.Format("capture_to_receive"));
    sserver::common::log::Logger::Info(decode_display_probe.decode_latency_recorder().Format("capture_to_decode"));
    sserver::common::log::Logger::Info(
            decode_display_probe.present_latency_recorder().Format("capture_to_present_ready"));

    decode_display_probe.Shutdown();
    bootstrap.Stop();
    sserver::common::log::Logger::Info("latency benchmark completed successfully");
    return 0;
}
