#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include <cstdint>
#include <cstring>
#include <string>
#include <thread>
#include <vector>

#include "modules/network/StreamClient.h"
#include "tests/support/SocketHelpers.h"
#include "tests/support/TestAssertions.h"

namespace {

using sclient::tests::support::Expect;
using sclient::tests::support::SendAll;

class TcpLoopbackServer {
public:
    ~TcpLoopbackServer() {
        Close();
    }

    bool Start(std::string *error_message) {
        Close();

        listen_fd_ = socket(AF_INET, SOCK_STREAM, 0);
        if (listen_fd_ < 0) {
            if (error_message != nullptr) {
                *error_message = "failed to create TCP listen socket";
            }
            return false;
        }

        int reuse = 1;
        setsockopt(listen_fd_, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));

        sockaddr_in address{};
        address.sin_family = AF_INET;
        address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        address.sin_port = 0;
        if (bind(listen_fd_, reinterpret_cast<const sockaddr *>(&address), sizeof(address)) != 0) {
            if (error_message != nullptr) {
                *error_message = "failed to bind TCP listen socket";
            }
            Close();
            return false;
        }

        if (listen(listen_fd_, 1) != 0) {
            if (error_message != nullptr) {
                *error_message = "failed to listen on TCP socket";
            }
            Close();
            return false;
        }

        sockaddr_in bound_address{};
        socklen_t address_length = sizeof(bound_address);
        if (getsockname(listen_fd_, reinterpret_cast<sockaddr *>(&bound_address), &address_length) != 0) {
            if (error_message != nullptr) {
                *error_message = "failed to read TCP listen address";
            }
            Close();
            return false;
        }

        port_ = ntohs(bound_address.sin_port);
        return true;
    }

    int port() const {
        return port_;
    }

    bool AcceptAndSendFrame(
            const sclient::protocol::FrameDiagnosticMetadata &metadata,
            const std::vector<std::uint8_t> &payload,
            std::string *error_message) {
        if (listen_fd_ < 0) {
            if (error_message != nullptr) {
                *error_message = "server is not listening";
            }
            return false;
        }

        sockaddr_in client_address{};
        socklen_t client_address_length = sizeof(client_address);
        client_fd_ = accept(listen_fd_, reinterpret_cast<sockaddr *>(&client_address), &client_address_length);
        if (client_fd_ < 0) {
            if (error_message != nullptr) {
                *error_message = "failed to accept TCP client";
            }
            return false;
        }

        sclient::protocol::MessageHeader header{};
        sclient::protocol::FillMessageMagic(header.head_id);
        header.message_type = static_cast<std::uint16_t>(sclient::protocol::MessageType::kAvStream);
        header.payload_length = static_cast<std::uint32_t>(sizeof(metadata) + payload.size());

        return SendAll(client_fd_, &header, sizeof(header)) &&
                SendAll(client_fd_, &metadata, sizeof(metadata)) &&
                (payload.empty() || SendAll(client_fd_, payload.data(), payload.size()));
    }

    void Close() {
        if (client_fd_ >= 0) {
            close(client_fd_);
            client_fd_ = -1;
        }
        if (listen_fd_ >= 0) {
            close(listen_fd_);
            listen_fd_ = -1;
        }
        port_ = 0;
    }

private:
    int listen_fd_ = -1;
    int client_fd_ = -1;
    int port_ = 0;
};

bool PayloadEquals(const std::vector<std::uint8_t> &payload, const std::string &expected) {
    return payload.size() == expected.size() &&
            std::memcmp(payload.data(), expected.data(), expected.size()) == 0;
}

bool TestTcpFrameMarksSenderMetadataAvailable() {
    TcpLoopbackServer server;
    std::string error_message;
    if (!server.Start(&error_message)) {
        return Expect(false, error_message);
    }

    const sclient::protocol::FrameDiagnosticMetadata metadata = {
            7,
            111000000ULL,
            112000000ULL,
            113000000ULL,
            114000000ULL,
    };
    const std::vector<std::uint8_t> payload = {'T', 'C', 'P', '!'};

    std::string server_error;
    std::thread server_thread([&]() {
        if (!server.AcceptAndSendFrame(metadata, payload, &server_error)) {
            if (server_error.empty()) {
                server_error = "failed to send TCP frame";
            }
        }
    });

    sclient::StreamClient client;
    sclient::ClientConfig config;
    config.host = "127.0.0.1";
    config.port = server.port();
    config.transport = "tcp";
    config.expect_metadata = true;
    if (!client.Connect(config, &error_message)) {
        server_thread.join();
        server.Close();
        return Expect(false, "failed to connect TCP client: " + error_message);
    }

    sclient::ReceivedFrame frame;
    const bool receive_ok = client.ReceiveFrame(&frame, &error_message);
    client.Close();
    server_thread.join();
    server.Close();

    if (!server_error.empty()) {
        return Expect(false, server_error);
    }
    if (!receive_ok) {
        return Expect(false, "failed to receive TCP frame: " + error_message);
    }

    return Expect(frame.sender_metadata_available, "expected TCP frame metadata to be marked available") &&
           Expect(frame.metadata.sequence == metadata.sequence, "expected TCP sequence metadata to round-trip") &&
           Expect(frame.metadata.capture_timestamp_ns == metadata.capture_timestamp_ns,
                  "expected TCP capture timestamp to round-trip") &&
           Expect(frame.metadata.transport_send_timestamp_ns == metadata.transport_send_timestamp_ns,
                  "expected TCP transport send timestamp to round-trip") &&
           Expect(PayloadEquals(frame.payload, "TCP!"), "expected TCP payload to round-trip");
}

}  // namespace

int main() {
    return TestTcpFrameMarksSenderMetadataAvailable() ? 0 : 1;
}
