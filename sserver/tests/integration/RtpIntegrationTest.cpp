#include <arpa/inet.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <unistd.h>

#include <cerrno>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

#include "app/AppBootstrap.h"
#include "common/log/Logger.h"
#include "common/net/H264AnnexB.h"
#include "common/net/RtpProtocol.h"
#include "common/time/MonotonicClock.h"
#include "config/AppConfig.h"

namespace {

struct Options {
    std::string config_path = std::string(SSERVER_SOURCE_DIR) + "/config/integration_rtp_null.conf";
};

struct ReceivedRtpPacket {
    sserver::common::net::RtpHeaderFields header;
    sserver::common::net::RtpHeaderExtension header_extension;
    sserver::common::net::RtpLatencyExtension latency_extension;
    std::uint64_t receive_timestamp_ns = 0;
    std::vector<std::uint8_t> payload;
};

bool ParseOptions(int argc, char **argv, Options *options) {
    if (options == nullptr) {
        return false;
    }

    for (int index = 1; index < argc; ++index) {
        if (std::string(argv[index]) == "--config" && index + 1 < argc) {
            options->config_path = argv[index + 1];
            ++index;
            continue;
        }
        return false;
    }

    return true;
}

bool IsV4L2DeviceAvailable(const std::string &device_path) {
    struct stat device_stat {};
    if (stat(device_path.c_str(), &device_stat) != 0) {
        return false;
    }
    return S_ISCHR(device_stat.st_mode);
}

bool OpenReceiverSocket(const sserver::config::TransportConfig &transport_config, int *socket_fd) {
    if (socket_fd == nullptr) {
        return false;
    }

    *socket_fd = socket(AF_INET, SOCK_DGRAM, 0);
    if (*socket_fd < 0) {
        return false;
    }

    int reuse = 1;
    setsockopt(*socket_fd, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));

    sockaddr_in address {};
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

    timeval timeout {};
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
    packet->receive_timestamp_ns = sserver::common::time::MonotonicNowNs();

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

bool CollectFrame(
        int socket_fd,
        std::uint8_t expected_payload_type,
        std::vector<ReceivedRtpPacket> *packets,
        std::vector<std::uint8_t> *annexb_payload,
        std::string *error_message) {
    if (packets == nullptr || annexb_payload == nullptr) {
        return false;
    }

    packets->clear();
    annexb_payload->clear();
    bool have_timestamp = false;
    std::uint32_t expected_timestamp = 0;
    bool fragment_open = false;

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
            have_timestamp = true;
        } else if (packet.header.timestamp != expected_timestamp) {
            if (error_message != nullptr) {
                *error_message = "received mixed RTP timestamps before frame marker";
            }
            return false;
        }

        if (!AppendRtpPayloadAsAnnexB(packet, annexb_payload, &fragment_open, error_message)) {
            return false;
        }
        packets->push_back(packet);
        if (packet.header.marker) {
            if (fragment_open) {
                if (error_message != nullptr) {
                    *error_message = "RTP frame ended while FU-A reassembly was still open";
                }
                return false;
            }
            return true;
        }
    }

    if (error_message != nullptr) {
        *error_message = "did not receive an RTP frame marker";
    }
    return false;
}

bool HasNalType(const std::vector<std::uint8_t> &annexb_payload, std::uint8_t nal_type) {
    const std::vector<sserver::common::net::H264NaluView> nalus =
            sserver::common::net::SplitAnnexBNalus(annexb_payload);
    for (std::size_t index = 0; index < nalus.size(); ++index) {
        if (nalus[index].nal_type() == nal_type) {
            return true;
        }
    }
    return false;
}

bool ValidatePacketSequence(const std::vector<ReceivedRtpPacket> &packets, std::string *error_message) {
    if (packets.empty()) {
        if (error_message != nullptr) {
            *error_message = "did not capture any RTP packets";
        }
        return false;
    }

    for (std::size_t index = 1; index < packets.size(); ++index) {
        const std::uint16_t expected =
                static_cast<std::uint16_t>(packets[index - 1].header.sequence_number + 1);
        if (packets[index].header.sequence_number != expected) {
            if (error_message != nullptr) {
                *error_message = "RTP sequence numbers are not contiguous";
            }
            return false;
        }
    }

    for (std::size_t index = 0; index + 1 < packets.size(); ++index) {
        if (packets[index].header.marker) {
            if (error_message != nullptr) {
                *error_message = "RTP marker bit was set before the final packet";
            }
            return false;
        }
    }
    if (!packets.back().header.marker) {
        if (error_message != nullptr) {
            *error_message = "final RTP packet was missing the marker bit";
        }
        return false;
    }

    return true;
}

bool ValidateLatencyExtension(
        const std::vector<ReceivedRtpPacket> &packets,
        std::uint32_t rtp_clock_rate,
        std::string *error_message) {
    if (packets.empty()) {
        if (error_message != nullptr) {
            *error_message = "did not capture any RTP packets for latency validation";
        }
        return false;
    }

    std::uint64_t expected_capture_timestamp_ns = 0;
    std::uint64_t previous_send_timestamp_ns = 0;
    for (std::size_t index = 0; index < packets.size(); ++index) {
        const ReceivedRtpPacket &packet = packets[index];
        if (packet.header_extension.profile_id != sserver::common::net::kRtpLatencyExtensionProfileId) {
            if (error_message != nullptr) {
                *error_message = "unexpected RTP header extension profile id";
            }
            return false;
        }
        if (packet.header_extension.payload.size() != sserver::common::net::kRtpLatencyExtensionPayloadSize) {
            if (error_message != nullptr) {
                *error_message = "unexpected RTP latency extension payload size";
            }
            return false;
        }
        if (packet.latency_extension.capture_timestamp_ns == 0 ||
            packet.latency_extension.transport_send_timestamp_ns == 0) {
            if (error_message != nullptr) {
                *error_message = "RTP latency extension timestamps must be non-zero";
            }
            return false;
        }
        if (packet.latency_extension.transport_send_timestamp_ns < packet.latency_extension.capture_timestamp_ns) {
            if (error_message != nullptr) {
                *error_message = "RTP send timestamp must not be earlier than capture timestamp";
            }
            return false;
        }
        if (packet.latency_extension.transport_send_timestamp_ns > packet.receive_timestamp_ns) {
            if (error_message != nullptr) {
                *error_message = "RTP send timestamp must not be later than local receive timestamp";
            }
            return false;
        }

        const std::uint32_t expected_rtp_timestamp = static_cast<std::uint32_t>(
                ((packet.latency_extension.capture_timestamp_ns * static_cast<std::uint64_t>(rtp_clock_rate)) /
                 1000000000ULL) &
                0xFFFFFFFFu);
        if (packet.header.timestamp != expected_rtp_timestamp) {
            if (error_message != nullptr) {
                *error_message = "RTP timestamp does not match capture timestamp encoded in extension";
            }
            return false;
        }

        if (index == 0) {
            expected_capture_timestamp_ns = packet.latency_extension.capture_timestamp_ns;
        } else {
            if (packet.latency_extension.capture_timestamp_ns != expected_capture_timestamp_ns) {
                if (error_message != nullptr) {
                    *error_message = "RTP packets from the same frame carried different capture timestamps";
                }
                return false;
            }
            if (packet.latency_extension.transport_send_timestamp_ns < previous_send_timestamp_ns) {
                if (error_message != nullptr) {
                    *error_message = "RTP send timestamps are not monotonic within a frame";
                }
                return false;
            }
        }

        previous_send_timestamp_ns = packet.latency_extension.transport_send_timestamp_ns;
    }

    return true;
}

bool ValidateSdpFile(const sserver::config::TransportConfig &transport_config, std::string *error_message) {
    std::ifstream input(transport_config.rtp_sdp_path.c_str());
    if (!input.is_open()) {
        if (error_message != nullptr) {
            *error_message = "failed to open generated SDP file: " + transport_config.rtp_sdp_path;
        }
        return false;
    }

    std::string contents((std::istreambuf_iterator<char>(input)), std::istreambuf_iterator<char>());
    const std::string media_line =
            "m=video " + std::to_string(transport_config.rtp_remote_port) +
            " RTP/AVP " + std::to_string(transport_config.rtp_payload_type);
    const std::string rtpmap_line =
            "a=rtpmap:" + std::to_string(transport_config.rtp_payload_type) +
            " H264/" + std::to_string(transport_config.rtp_clock_rate);
    if (contents.find(media_line) == std::string::npos ||
        contents.find(rtpmap_line) == std::string::npos ||
        contents.find("packetization-mode=1") == std::string::npos) {
        if (error_message != nullptr) {
            *error_message = "generated SDP file is missing expected H264 RTP fields";
        }
        return false;
    }

    return true;
}

}  // namespace

int main(int argc, char **argv) {
    Options options;
    if (!ParseOptions(argc, argv, &options)) {
        sserver::common::log::Logger::Error("invalid arguments");
        return 1;
    }

    sserver::config::AppConfig config;
    std::string error_message;
    if (!sserver::config::ConfigLoader::LoadFromFile(options.config_path, &config, &error_message)) {
        sserver::common::log::Logger::Error("failed to load config: " + error_message);
        return 2;
    }
    if (config.transport.backend != "rtp") {
        sserver::common::log::Logger::Error("rtp integration test requires transport.backend=rtp");
        return 3;
    }
    if (config.capture.source == "v4l2" && !IsV4L2DeviceAvailable(config.capture.device)) {
        sserver::common::log::Logger::Warn("v4l2 capture device is unavailable, skipping RTP integration test");
        return 77;
    }

    if (!config.transport.rtp_sdp_path.empty()) {
        std::remove(config.transport.rtp_sdp_path.c_str());
    }

    int socket_fd = -1;
    if (!OpenReceiverSocket(config.transport, &socket_fd)) {
        sserver::common::log::Logger::Error("failed to open RTP receiver socket");
        return 4;
    }

    sserver::app::AppBootstrap bootstrap(config);
    if (!bootstrap.Start()) {
        sserver::common::log::Logger::Error("failed to start application");
        close(socket_fd);
        return 5;
    }

    bool validated_frame = false;
    for (int attempt = 0; attempt < 8 && !validated_frame; ++attempt) {
        std::vector<ReceivedRtpPacket> packets;
        std::vector<std::uint8_t> annexb_payload;
        error_message.clear();
        if (!CollectFrame(
                    socket_fd,
                    static_cast<std::uint8_t>(config.transport.rtp_payload_type),
                    &packets,
                    &annexb_payload,
                    &error_message)) {
            sserver::common::log::Logger::Error("failed to collect RTP frame: " + error_message);
            close(socket_fd);
            bootstrap.Stop();
            return 6;
        }
        if (!ValidatePacketSequence(packets, &error_message)) {
            sserver::common::log::Logger::Error("invalid RTP packet sequence: " + error_message);
            close(socket_fd);
            bootstrap.Stop();
            return 7;
        }
        if (!ValidateLatencyExtension(
                    packets,
                    static_cast<std::uint32_t>(config.transport.rtp_clock_rate),
                    &error_message)) {
            sserver::common::log::Logger::Error("invalid RTP latency extension: " + error_message);
            close(socket_fd);
            bootstrap.Stop();
            return 10;
        }

        if (HasNalType(annexb_payload, 7) && HasNalType(annexb_payload, 8) && HasNalType(annexb_payload, 5)) {
            validated_frame = true;
        }
    }

    if (!validated_frame) {
        sserver::common::log::Logger::Error("did not observe an RTP/H264 keyframe with SPS/PPS");
        close(socket_fd);
        bootstrap.Stop();
        return 8;
    }

    if (!ValidateSdpFile(config.transport, &error_message)) {
        sserver::common::log::Logger::Error("invalid generated SDP file: " + error_message);
        close(socket_fd);
        bootstrap.Stop();
        return 9;
    }

    close(socket_fd);
    bootstrap.Stop();
    sserver::common::log::Logger::Info("rtp integration test completed successfully");
    return 0;
}
