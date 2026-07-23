#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include <algorithm>
#include <chrono>
#include <cerrno>
#include <cstdlib>
#include <cstring>
#include <iomanip>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

#include "app/cli/CliOptions.h"
#include "common/log/Logger.h"
#include "common/net/RtpProtocol.h"
#include "modules/decoding/VideoDecoder.h"
#include "modules/network/StreamClient.h"
#include "tests/support/H264Samples.h"

namespace {

using sclient::common::log::Logger;

void PrintUsage(const char *program_name) {
    std::ostringstream stream;
    stream << "Usage: " << program_name << " [options]\n"
           << "  --host <ip>                 local bind address, default: 127.0.0.1\n"
           << "  --port <port>               local bind port, default: auto in self-test mode\n"
           << "  --frames <n>                decoded frames target, default: 3\n"
           << "  --max-receive <n>           received frames limit, default: 12\n"
           << "  --decoder <auto|software>   default: auto\n"
           << "  --self-test <on|off>        default: on\n"
           << "  --help";
    Logger::Info(stream.str());
}

std::string BuildSocketError(const std::string &message) {
    return message + ": " + std::strerror(errno);
}

std::string FormatLeadingBytes(const std::vector<std::uint8_t> &data, std::size_t max_bytes) {
    std::ostringstream stream;
    stream << std::hex << std::setfill('0');
    const std::size_t count = std::min(max_bytes, data.size());
    for (std::size_t index = 0; index < count; ++index) {
        if (index > 0) {
            stream << ' ';
        }
        stream << std::setw(2) << static_cast<unsigned int>(data[index]);
    }
    return stream.str();
}

bool ReserveLoopbackUdpPort(int *port, std::string *error_message) {
    if (port == nullptr) {
        return false;
    }

    *port = 0;
    const int socket_fd = socket(AF_INET, SOCK_DGRAM, 0);
    if (socket_fd < 0) {
        if (error_message != nullptr) {
            *error_message = BuildSocketError("failed to create UDP socket while reserving smoke-test port");
        }
        return false;
    }

    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_port = 0;
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    if (bind(socket_fd, reinterpret_cast<const sockaddr *>(&address), sizeof(address)) != 0) {
        if (error_message != nullptr) {
            *error_message = BuildSocketError("failed to bind smoke-test UDP socket");
        }
        close(socket_fd);
        return false;
    }

    sockaddr_in bound_address{};
    socklen_t address_length = sizeof(bound_address);
    if (getsockname(socket_fd, reinterpret_cast<sockaddr *>(&bound_address), &address_length) != 0) {
        if (error_message != nullptr) {
            *error_message = BuildSocketError("failed to query smoke-test UDP port");
        }
        close(socket_fd);
        return false;
    }

    *port = ntohs(bound_address.sin_port);
    close(socket_fd);
    return true;
}

std::vector<std::uint8_t> BuildSingleNaluRtpPacket(
        const std::vector<std::uint8_t> &nalu,
        std::uint8_t payload_type,
        std::uint16_t sequence_number,
        std::uint32_t timestamp,
        std::uint32_t ssrc,
        bool marker) {
    sclient::common::net::RtpHeaderFields header_fields;
    header_fields.marker = marker;
    header_fields.payload_type = payload_type;
    header_fields.sequence_number = sequence_number;
    header_fields.timestamp = timestamp;
    header_fields.ssrc = ssrc;

    std::vector<std::uint8_t> packet;
    sclient::common::net::WriteRtpHeader(header_fields, &packet);
    packet.insert(packet.end(), nalu.begin(), nalu.end());
    return packet;
}

bool RunEmbeddedSelfTestSender(
        const sclient::ClientConfig &config,
        int frame_count,
        std::string *error_message) {
    const std::vector<std::vector<std::uint8_t> > nalus = sclient::tests::support::EmbeddedH264IdrFrameNalus();

    const int socket_fd = socket(AF_INET, SOCK_DGRAM, 0);
    if (socket_fd < 0) {
        if (error_message != nullptr) {
            *error_message = BuildSocketError("failed to create RTP smoke-test sender socket");
        }
        return false;
    }

    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_port = htons(static_cast<std::uint16_t>(config.port));
    if (inet_pton(AF_INET, config.host.c_str(), &address.sin_addr) != 1) {
        if (error_message != nullptr) {
            *error_message = "invalid smoke-test RTP host: " + config.host;
        }
        close(socket_fd);
        return false;
    }

    const std::uint8_t payload_type = static_cast<std::uint8_t>(config.rtp_payload_type >= 0 ? config.rtp_payload_type : 96);
    std::uint16_t sequence_number = 1000;
    std::uint32_t timestamp = 90000;
    const std::uint32_t ssrc = 0x534D4F4B;  // "SMOK"

    std::this_thread::sleep_for(std::chrono::milliseconds(10));
    for (int frame_index = 0; frame_index < frame_count; ++frame_index) {
        for (std::size_t nalu_index = 0; nalu_index < nalus.size(); ++nalu_index) {
            const std::vector<std::uint8_t> packet = BuildSingleNaluRtpPacket(
                    nalus[nalu_index],
                    payload_type,
                    sequence_number++,
                    timestamp,
                    ssrc,
                    nalu_index + 1 == nalus.size());
            const ssize_t sent = sendto(
                    socket_fd,
                    packet.data(),
                    packet.size(),
                    0,
                    reinterpret_cast<const sockaddr *>(&address),
                    sizeof(address));
            if (sent != static_cast<ssize_t>(packet.size())) {
                if (error_message != nullptr) {
                    *error_message = BuildSocketError("failed to send embedded RTP smoke-test packet");
                }
                close(socket_fd);
                return false;
            }
        }

        timestamp += 3000;  // 30 fps clock step at 90 kHz.
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }

    close(socket_fd);
    return true;
}

}  // namespace

int main(int argc, char **argv) {
    sclient::ClientConfig config;
    config.transport = "rtp";
    config.expect_metadata = false;
    sclient::DecodeBackend decode_backend = sclient::DecodeBackend::kAuto;
    int target_decoded_frames = 3;
    int max_received_frames = 12;
    bool self_test_enabled = true;
    bool host_was_explicit = false;
    bool port_was_explicit = false;
    std::string error_message;

    for (int index = 1; index < argc; ++index) {
        if (std::strcmp(argv[index], "--host") == 0) {
            host_was_explicit = true;
        }
        if (std::strcmp(argv[index], "--port") == 0) {
            port_was_explicit = true;
        }

        const sclient::CliParseResult shared_result =
                sclient::ParseSharedStreamOption(argc, argv, &index, &config, &decode_backend);
        if (shared_result.show_help) {
            PrintUsage(argv[0]);
            return 0;
        }
        if (shared_result.handled) {
            if (!shared_result.success) {
                Logger::Error(shared_result.error_message);
                return 1;
            }
            continue;
        }

        if (std::strcmp(argv[index], "--self-test") == 0) {
            if (index + 1 >= argc) {
                Logger::Error("missing value for --self-test");
                return 1;
            }
            if (!sclient::ParseBoolFlag(argv[++index], &self_test_enabled)) {
                Logger::Error("invalid value for --self-test");
                return 1;
            }
            continue;
        }
        if (std::strcmp(argv[index], "--frames") == 0) {
            if (index + 1 >= argc) {
                Logger::Error("missing value for --frames");
                return 1;
            }
            target_decoded_frames = std::max(1, std::atoi(argv[++index]));
            continue;
        }
        if (std::strcmp(argv[index], "--max-receive") == 0) {
            if (index + 1 >= argc) {
                Logger::Error("missing value for --max-receive");
                return 1;
            }
            max_received_frames = std::max(1, std::atoi(argv[++index]));
            continue;
        }
        if (std::strcmp(argv[index], "--help") == 0) {
            PrintUsage(argv[0]);
            return 0;
        }

        PrintUsage(argv[0]);
        return 1;
    }

    if (self_test_enabled) {
        if (host_was_explicit && config.host != "127.0.0.1") {
            Logger::Error("embedded RTP smoke test only supports --host 127.0.0.1");
            return 1;
        }
        config.host = "127.0.0.1";
        if (!port_was_explicit && !ReserveLoopbackUdpPort(&config.port, &error_message)) {
            Logger::Error(error_message);
            return 1;
        }
    }

    sclient::StreamClient client;
    if (!client.Connect(config, &error_message)) {
        Logger::Error("failed to connect stream client: " + error_message);
        return 2;
    }

    sclient::VideoDecoder decoder;
    if (!decoder.Initialize(decode_backend, &error_message)) {
        Logger::Error("failed to initialize decoder: " + error_message);
        return 3;
    }

    std::string sender_error_message;
    std::thread sender_thread;
    if (self_test_enabled) {
        const int frames_to_send = std::max(target_decoded_frames * 2, target_decoded_frames + 2);
        sender_thread = std::thread([&]() {
            if (!RunEmbeddedSelfTestSender(config, frames_to_send, &sender_error_message) &&
                sender_error_message.empty()) {
                sender_error_message = "embedded RTP smoke-test sender failed";
            }
        });
    }

    int received_frames = 0;
    int decoded_frames = 0;
    int last_width = 0;
    int last_height = 0;
    bool logged_decode_failure_payload = false;

    while (received_frames < max_received_frames && decoded_frames < target_decoded_frames) {
        sclient::ReceivedFrame frame;
        if (!client.ReceiveFrame(&frame, &error_message)) {
            client.Close();
            if (sender_thread.joinable()) {
                sender_thread.join();
            }
            if (!sender_error_message.empty()) {
                Logger::Error("embedded RTP sender failed: " + sender_error_message);
                return 6;
            }
            Logger::Error("stream receive failed: " + error_message);
            return 4;
        }
        ++received_frames;

        sclient::DecodedFrame decoded_frame;
        if (!decoder.Decode(frame.payload.data(), frame.payload.size(), &decoded_frame, &error_message)) {
            if (self_test_enabled && !logged_decode_failure_payload) {
                std::ostringstream stream;
                stream << "rtp_decode_smoke decode miss"
                       << " payload_size=" << frame.payload.size()
                       << " leading_bytes=" << FormatLeadingBytes(frame.payload, 32)
                       << " decoder_error=" << error_message;
                Logger::Info(stream.str());
                logged_decode_failure_payload = true;
            }
            continue;
        }

        ++decoded_frames;
        last_width = decoded_frame.width;
        last_height = decoded_frame.height;
    }

    client.Close();
    if (sender_thread.joinable()) {
        sender_thread.join();
    }
    if (!sender_error_message.empty()) {
        Logger::Error("embedded RTP sender failed: " + sender_error_message);
        return 6;
    }

    if (decoded_frames < target_decoded_frames) {
        std::ostringstream stream;
        stream << "rtp_decode_smoke failed"
               << " self_test=" << (self_test_enabled ? "on" : "off")
               << " received=" << received_frames
               << " decoded=" << decoded_frames
               << " target=" << target_decoded_frames;
        Logger::Error(stream.str());
        return 5;
    }

    std::ostringstream stream;
    stream << "rtp_decode_smoke passed"
           << " self_test=" << (self_test_enabled ? "on" : "off")
           << " bind=" << config.host << ":" << config.port
           << " received=" << received_frames
           << " decoded=" << decoded_frames
           << " resolution=" << last_width << "x" << last_height;
    Logger::Info(stream.str());
    return 0;
}
