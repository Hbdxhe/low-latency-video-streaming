#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>

#include <chrono>
#include <cstdint>
#include <cstring>
#include <string>
#include <thread>
#include <vector>

#include "modules/network/StreamClient.h"
#include "tests/support/MonotonicClock.h"
#include "tests/support/TestAssertions.h"

namespace {

using Clock = std::chrono::steady_clock;
using sclient::tests::support::Expect;
using sclient::tests::support::MonotonicNowNs;

class UdpLoopbackServer {
public:
    UdpLoopbackServer() = default;

    ~UdpLoopbackServer() {
        Close();
    }

    bool Start(std::string *error_message) {
        Close();

        socket_fd_ = socket(AF_INET, SOCK_DGRAM, 0);
        if (socket_fd_ < 0) {
            if (error_message != nullptr) {
                *error_message = "failed to create UDP socket";
            }
            return false;
        }

        timeval timeout{};
        timeout.tv_sec = 0;
        timeout.tv_usec = 100 * 1000;
        setsockopt(socket_fd_, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));

        sockaddr_in address{};
        address.sin_family = AF_INET;
        address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        address.sin_port = 0;
        if (bind(socket_fd_, reinterpret_cast<const sockaddr *>(&address), sizeof(address)) != 0) {
            if (error_message != nullptr) {
                *error_message = "failed to bind UDP socket";
            }
            Close();
            return false;
        }

        sockaddr_in bound_address{};
        socklen_t address_length = sizeof(bound_address);
        if (getsockname(socket_fd_, reinterpret_cast<sockaddr *>(&bound_address), &address_length) != 0) {
            if (error_message != nullptr) {
                *error_message = "failed to read bound UDP socket address";
            }
            Close();
            return false;
        }

        port_ = ntohs(bound_address.sin_port);
        return true;
    }

    void Close() {
        if (socket_fd_ >= 0) {
            close(socket_fd_);
            socket_fd_ = -1;
        }
        client_address_length_ = 0;
        std::memset(&client_address_, 0, sizeof(client_address_));
        port_ = 0;
    }

    int port() const {
        return port_;
    }

    bool WaitForClient(std::vector<std::uint8_t> *packet, int timeout_ms, std::string *error_message) {
        const Clock::time_point deadline = Clock::now() + std::chrono::milliseconds(timeout_ms);
        while (Clock::now() < deadline) {
            sockaddr_in address{};
            socklen_t address_length = sizeof(address);
            std::vector<std::uint8_t> received_packet;
            if (!ReceivePacketFrom(&address, &address_length, &received_packet, error_message)) {
                continue;
            }

            client_address_ = address;
            client_address_length_ = address_length;
            if (packet != nullptr) {
                *packet = received_packet;
            }
            return true;
        }

        if (error_message != nullptr && error_message->empty()) {
            *error_message = "timed out waiting for initial client UDP packet";
        }
        return false;
    }

    bool SendPacket(const std::vector<std::uint8_t> &packet, std::string *error_message) {
        if (socket_fd_ < 0 || client_address_length_ == 0) {
            if (error_message != nullptr) {
                *error_message = "server is not connected to a client endpoint";
            }
            return false;
        }

        const ssize_t sent = sendto(
                socket_fd_,
                packet.data(),
                packet.size(),
                0,
                reinterpret_cast<const sockaddr *>(&client_address_),
                client_address_length_);
        if (sent != static_cast<ssize_t>(packet.size())) {
            if (error_message != nullptr) {
                *error_message = "failed to send UDP packet to client";
            }
            return false;
        }
        return true;
    }

    bool ReceiveMessageOfType(
            sclient::protocol::MessageType message_type,
            std::vector<std::uint8_t> *packet,
            int timeout_ms,
            std::string *error_message) {
        const Clock::time_point deadline = Clock::now() + std::chrono::milliseconds(timeout_ms);
        while (Clock::now() < deadline) {
            sockaddr_in address{};
            socklen_t address_length = sizeof(address);
            std::vector<std::uint8_t> received_packet;
            if (!ReceivePacketFrom(&address, &address_length, &received_packet, error_message)) {
                continue;
            }
            if (received_packet.size() < sizeof(sclient::protocol::MessageHeader)) {
                continue;
            }

            sclient::protocol::MessageHeader header{};
            std::memcpy(&header, received_packet.data(), sizeof(header));
            if (!sclient::protocol::HasValidMessageMagic(header)) {
                continue;
            }
            if (header.message_type != static_cast<std::uint16_t>(message_type)) {
                continue;
            }

            client_address_ = address;
            client_address_length_ = address_length;
            if (packet != nullptr) {
                *packet = received_packet;
            }
            return true;
        }

        if (error_message != nullptr && error_message->empty()) {
            *error_message = "timed out waiting for UDP control packet";
        }
        return false;
    }

private:
    bool ReceivePacketFrom(
            sockaddr_in *address,
            socklen_t *address_length,
            std::vector<std::uint8_t> *packet,
            std::string *error_message) {
        if (socket_fd_ < 0 || address == nullptr || address_length == nullptr || packet == nullptr) {
            return false;
        }

        std::uint8_t buffer[2048];
        const ssize_t received = recvfrom(
                socket_fd_,
                buffer,
                sizeof(buffer),
                0,
                reinterpret_cast<sockaddr *>(address),
                address_length);
        if (received < 0) {
            return false;
        }
        if (received == 0) {
            return false;
        }

        packet->assign(buffer, buffer + static_cast<std::size_t>(received));
        if (error_message != nullptr) {
            error_message->clear();
        }
        return true;
    }

private:
    int socket_fd_ = -1;
    int port_ = 0;
    sockaddr_in client_address_{};
    socklen_t client_address_length_ = 0;
};

std::vector<std::uint8_t> BuildUdpFragmentPacket(
        std::uint64_t frame_sequence,
        const std::vector<std::uint8_t> &payload,
        std::uint32_t frame_payload_size,
        std::uint32_t fragment_offset,
        std::uint16_t fragment_index,
        std::uint16_t fragment_count,
        sclient::protocol::UdpFragmentRole fragment_role) {
    sclient::protocol::MessageHeader header{};
    sclient::protocol::FillMessageMagic(header.head_id);
    header.message_type = static_cast<std::uint16_t>(sclient::protocol::MessageType::kAvStream);
    header.payload_length = static_cast<std::uint32_t>(sizeof(sclient::protocol::UdpFrameFragmentHeader) + payload.size());

    sclient::protocol::UdpFrameFragmentHeader fragment_header{};
    fragment_header.frame_sequence = frame_sequence;
    fragment_header.capture_timestamp_ns = MonotonicNowNs();
    fragment_header.encode_start_timestamp_ns = fragment_header.capture_timestamp_ns + 1000;
    fragment_header.encode_end_timestamp_ns = fragment_header.capture_timestamp_ns + 2000;
    fragment_header.transport_send_timestamp_ns = fragment_header.capture_timestamp_ns + 3000;
    fragment_header.frame_payload_size = frame_payload_size;
    fragment_header.fragment_offset = fragment_offset;
    fragment_header.fragment_index = fragment_index;
    fragment_header.fragment_count = fragment_count;
    fragment_header.fragment_role = static_cast<std::uint16_t>(fragment_role);

    std::vector<std::uint8_t> packet(sizeof(header) + sizeof(fragment_header) + payload.size());
    std::memcpy(packet.data(), &header, sizeof(header));
    std::memcpy(packet.data() + sizeof(header), &fragment_header, sizeof(fragment_header));
    if (!payload.empty()) {
        std::memcpy(packet.data() + sizeof(header) + sizeof(fragment_header), payload.data(), payload.size());
    }
    return packet;
}

std::vector<std::uint8_t> BuildXorParity(
        const std::vector<std::uint8_t> &left,
        const std::vector<std::uint8_t> &right) {
    const std::size_t size = std::max(left.size(), right.size());
    std::vector<std::uint8_t> parity(size, 0);
    for (std::size_t index = 0; index < size; ++index) {
        const std::uint8_t left_value = index < left.size() ? left[index] : 0;
        const std::uint8_t right_value = index < right.size() ? right[index] : 0;
        parity[index] = static_cast<std::uint8_t>(left_value ^ right_value);
    }
    return parity;
}

bool PayloadEquals(const std::vector<std::uint8_t> &payload, const std::string &expected) {
    return payload.size() == expected.size() &&
            std::memcmp(payload.data(), expected.data(), expected.size()) == 0;
}

bool PrepareClientAndServer(
        sclient::StreamClient *client,
        UdpLoopbackServer *server,
        const sclient::ClientConfig &config,
        std::string *error_message) {
    if (client == nullptr || server == nullptr) {
        if (error_message != nullptr) {
            *error_message = "client/server pointer is null";
        }
        return false;
    }

    if (!server->Start(error_message)) {
        return false;
    }

    sclient::ClientConfig client_config = config;
    client_config.host = "127.0.0.1";
    client_config.port = server->port();
    client_config.transport = "udp";
    if (!client->Connect(client_config, error_message)) {
        return false;
    }

    std::vector<std::uint8_t> initial_packet;
    return server->WaitForClient(&initial_packet, 1000, error_message);
}

bool TestJitterBufferSkipsMissingFrame() {
    sclient::StreamClient client;
    UdpLoopbackServer server;
    std::string error_message;
    sclient::ClientConfig config;
    config.expect_metadata = true;
    config.udp_jitter_buffer_enabled = true;
    config.udp_jitter_buffer_target_delay_ms = 0;
    config.udp_jitter_buffer_max_wait_ms = 0;
    config.udp_jitter_buffer_max_frames = 1;
    config.udp_nack_enabled = false;
    config.udp_fec_enabled = false;

    if (!PrepareClientAndServer(&client, &server, config, &error_message)) {
        return Expect(false, error_message);
    }

    const std::vector<std::uint8_t> frame1_payload = {'A', 'A', 'A', 'A'};
    const std::vector<std::uint8_t> frame3_payload = {'C', 'C', 'C', 'C'};
    if (!server.SendPacket(
                BuildUdpFragmentPacket(1, frame1_payload, frame1_payload.size(), 0, 0, 1, sclient::protocol::UdpFragmentRole::kData),
                &error_message) ||
        !server.SendPacket(
                BuildUdpFragmentPacket(3, frame3_payload, frame3_payload.size(), 0, 0, 1, sclient::protocol::UdpFragmentRole::kData),
                &error_message)) {
        client.Close();
        return Expect(false, error_message);
    }

    sclient::ReceivedFrame first_frame;
    if (!client.ReceiveFrame(&first_frame, &error_message)) {
        client.Close();
        return Expect(false, "failed to receive first jitter-buffered frame: " + error_message);
    }
    if (!Expect(first_frame.metadata.sequence == 1, "expected first frame sequence to be 1")) {
        client.Close();
        return false;
    }
    if (!Expect(PayloadEquals(first_frame.payload, "AAAA"), "expected first frame payload to match")) {
        client.Close();
        return false;
    }

    sclient::ReceivedFrame third_frame;
    if (!client.ReceiveFrame(&third_frame, &error_message)) {
        client.Close();
        return Expect(false, "failed to receive skipped jitter-buffer frame: " + error_message);
    }
    if (!Expect(third_frame.metadata.sequence == 3, "expected second emitted frame sequence to be 3")) {
        client.Close();
        return false;
    }
    if (!Expect(PayloadEquals(third_frame.payload, "CCCC"), "expected third frame payload to match")) {
        client.Close();
        return false;
    }

    const sclient::UdpReceiveStats stats = client.udp_receive_stats();
    client.Close();
    return Expect(stats.jitter_buffer_skipped_frames == 1, "expected jitter buffer to skip exactly one frame");
}

bool TestFecRecovery() {
    sclient::StreamClient client;
    UdpLoopbackServer server;
    std::string error_message;
    sclient::ClientConfig config;
    config.expect_metadata = true;
    config.udp_jitter_buffer_enabled = false;
    config.udp_nack_enabled = false;
    config.udp_fec_enabled = true;

    if (!PrepareClientAndServer(&client, &server, config, &error_message)) {
        return Expect(false, error_message);
    }

    const std::vector<std::uint8_t> fragment0 = {'A', 'B', 'C', 'D'};
    const std::vector<std::uint8_t> fragment1 = {'W', 'X', 'Y', 'Z'};
    const std::vector<std::uint8_t> parity = BuildXorParity(fragment0, fragment1);

    if (!server.SendPacket(
                BuildUdpFragmentPacket(10, fragment0, 8, 0, 0, 2, sclient::protocol::UdpFragmentRole::kData),
                &error_message) ||
        !server.SendPacket(
                BuildUdpFragmentPacket(10, parity, 8, 0, 0, 2, sclient::protocol::UdpFragmentRole::kXorParity),
                &error_message)) {
        client.Close();
        return Expect(false, error_message);
    }

    sclient::ReceivedFrame frame;
    if (!client.ReceiveFrame(&frame, &error_message)) {
        client.Close();
        return Expect(false, "failed to receive FEC-recovered frame: " + error_message);
    }
    if (!Expect(frame.metadata.sequence == 10, "expected FEC frame sequence to be 10")) {
        client.Close();
        return false;
    }
    if (!Expect(frame.sender_metadata_available, "expected UDP frame metadata to be marked available")) {
        client.Close();
        return false;
    }
    if (!Expect(PayloadEquals(frame.payload, "ABCDWXYZ"), "expected FEC recovery payload to match")) {
        client.Close();
        return false;
    }

    const sclient::UdpReceiveStats stats = client.udp_receive_stats();
    client.Close();
    return Expect(stats.fec_recovered_frames == 1, "expected exactly one FEC-recovered frame");
}

bool TestNackRecovery() {
    sclient::StreamClient client;
    UdpLoopbackServer server;
    std::string error_message;
    sclient::ClientConfig config;
    config.expect_metadata = true;
    config.udp_jitter_buffer_enabled = false;
    config.udp_nack_enabled = true;
    config.udp_nack_request_delay_ms = 5;
    config.udp_nack_retry_interval_ms = 10;
    config.udp_nack_max_retries = 2;
    config.udp_fec_enabled = false;

    if (!PrepareClientAndServer(&client, &server, config, &error_message)) {
        return Expect(false, error_message);
    }

    const std::vector<std::uint8_t> fragment0 = {'L', 'M', 'N', 'O'};
    const std::vector<std::uint8_t> fragment1 = {'P', 'Q', 'R', 'S'};
    bool received_nack = false;
    std::string server_thread_error;

    std::thread server_thread([&]() {
        if (!server.SendPacket(
                    BuildUdpFragmentPacket(20, fragment0, 8, 0, 0, 2, sclient::protocol::UdpFragmentRole::kData),
                    &server_thread_error)) {
            return;
        }

        std::this_thread::sleep_for(std::chrono::milliseconds(20));
        if (!server.SendPacket(
                    BuildUdpFragmentPacket(20, fragment0, 8, 0, 0, 2, sclient::protocol::UdpFragmentRole::kData),
                    &server_thread_error)) {
            return;
        }

        std::vector<std::uint8_t> nack_packet;
        if (!server.ReceiveMessageOfType(sclient::protocol::MessageType::kUdpNack, &nack_packet, 1000, &server_thread_error)) {
            return;
        }

        if (nack_packet.size() < sizeof(sclient::protocol::MessageHeader) +
                                 sizeof(sclient::protocol::UdpNackHeader) +
                                 sizeof(sclient::protocol::UdpNackItem)) {
            server_thread_error = "received short NACK packet";
            return;
        }

        sclient::protocol::UdpNackHeader nack_header{};
        std::memcpy(
                &nack_header,
                nack_packet.data() + sizeof(sclient::protocol::MessageHeader),
                sizeof(nack_header));
        if (nack_header.request_count == 0) {
            server_thread_error = "received empty NACK request";
            return;
        }

        const std::uint8_t *item_data = nack_packet.data() +
                sizeof(sclient::protocol::MessageHeader) +
                sizeof(sclient::protocol::UdpNackHeader);
        for (std::uint16_t index = 0; index < nack_header.request_count; ++index) {
            sclient::protocol::UdpNackItem item{};
            std::memcpy(&item, item_data + index * sizeof(item), sizeof(item));
            if (item.frame_sequence == 20 && item.fragment_index == 1) {
                received_nack = true;
                break;
            }
        }
        if (!received_nack) {
            server_thread_error = "did not receive expected fragment-1 NACK";
            return;
        }

        server.SendPacket(
                BuildUdpFragmentPacket(20, fragment1, 8, 4, 1, 2, sclient::protocol::UdpFragmentRole::kData),
                &server_thread_error);
    });

    sclient::ReceivedFrame frame;
    const bool receive_ok = client.ReceiveFrame(&frame, &error_message);
    server_thread.join();
    if (!server_thread_error.empty()) {
        client.Close();
        return Expect(false, server_thread_error);
    }
    if (!receive_ok) {
        client.Close();
        return Expect(false, "failed to receive NACK-recovered frame: " + error_message);
    }
    if (!Expect(received_nack, "expected test server to observe a NACK request")) {
        client.Close();
        return false;
    }
    if (!Expect(frame.metadata.sequence == 20, "expected NACK-recovered frame sequence to be 20")) {
        client.Close();
        return false;
    }
    if (!Expect(PayloadEquals(frame.payload, "LMNOPQRS"), "expected NACK-recovered payload to match")) {
        client.Close();
        return false;
    }

    const sclient::UdpReceiveStats stats = client.udp_receive_stats();
    client.Close();
    return Expect(stats.nack_requests_sent >= 1 && stats.nack_fragments_requested >= 1,
                  "expected client NACK stats to record at least one request");
}

}  // namespace

int main(int argc, char **argv) {
    const std::string selected_test = argc > 1 ? argv[1] : "all";

    if ((selected_test == "all" || selected_test == "jitter") &&
        !TestJitterBufferSkipsMissingFrame()) {
        return 1;
    }
    if ((selected_test == "all" || selected_test == "fec") &&
        !TestFecRecovery()) {
        return 1;
    }
    if ((selected_test == "all" || selected_test == "nack") &&
        !TestNackRecovery()) {
        return 1;
    }
    return 0;
}
