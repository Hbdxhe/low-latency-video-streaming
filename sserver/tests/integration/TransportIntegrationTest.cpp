#include <string>

#include "app/AppBootstrap.h"
#include "common/log/Logger.h"
#include "common/net/StreamProtocol.h"
#include "config/AppConfig.h"
#include "tests/support/TransportTestClient.h"

namespace {

std::string ParseConfigPath(int argc, char **argv) {
    std::string config_path = std::string(SSERVER_SOURCE_DIR) + "/config/integration_tcp.conf";
    for (int index = 1; index < argc; ++index) {
        if (std::string(argv[index]) == "--config" && index + 1 < argc) {
            config_path = argv[index + 1];
            ++index;
        }
    }
    return config_path;
}

bool PayloadHasExpectedPrefix(const std::vector<std::uint8_t> &payload) {
    const std::string text(payload.begin(), payload.end());
    return text.find("null-frame-") == 0;
}

}  // namespace

int main(int argc, char **argv) {
    const std::string config_path = ParseConfigPath(argc, argv);

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
    sserver::common::log::Logger::Info("integration transport test starting with config: " + config_path);
    if (!config.transport.embed_frame_metadata) {
        sserver::common::log::Logger::Error(
                "integration transport test requires transport.embed_frame_metadata=true");
        return 2;
    }

    sserver::app::AppBootstrap bootstrap(config);
    if (!bootstrap.Start()) {
        sserver::common::log::Logger::Error("failed to start application");
        return 3;
    }

    int port = 0;
    if (!sserver::tests::support::WaitForBoundPort(bootstrap, &port)) {
        sserver::common::log::Logger::Error("integration transport test timed out waiting for bound port");
        bootstrap.Stop();
        return 4;
    }
    sserver::common::log::Logger::Info("integration transport test bound to port " + std::to_string(port));

    int socket_fd = -1;
    if (!sserver::tests::support::ConnectClient(config.transport, port, &socket_fd)) {
        sserver::common::log::Logger::Error("integration transport test failed to connect client");
        bootstrap.Stop();
        return 5;
    }

    const int frames_to_validate = 5;
    std::uint64_t previous_sequence = 0;
    bool nack_recovery_exercised = false;
    bool fec_recovery_exercised = false;
    for (int frame_index = 0; frame_index < frames_to_validate; ++frame_index) {
        if (!sserver::tests::support::RefreshClientKeepAlive(socket_fd, config.transport, frame_index)) {
            close(socket_fd);
            bootstrap.Stop();
            return 6;
        }

        sserver::tests::support::ReceivedFrame frame;
        bool receive_ok = false;
        if (config.transport.backend == "udp" && frame_index == 0) {
            if (config.transport.udp_enable_fec && !config.transport.udp_enable_nack) {
                receive_ok = sserver::tests::support::ReceiveUdpFrameWithFecRecovery(
                        socket_fd,
                        config.transport.udp_max_datagram_size,
                        config.transport.embed_frame_metadata,
                        &frame,
                        &fec_recovery_exercised);
            } else {
                receive_ok = sserver::tests::support::ReceiveUdpFrameWithNackRecovery(
                        socket_fd,
                        config.transport.udp_max_datagram_size,
                        config.transport.embed_frame_metadata,
                        &frame,
                        &nack_recovery_exercised);
            }
        } else {
            receive_ok = sserver::tests::support::ReceiveFrame(socket_fd, config.transport, &frame);
        }
        if (!receive_ok) {
            sserver::common::log::Logger::Error("integration transport test failed while receiving frame");
            close(socket_fd);
            bootstrap.Stop();
            return 7;
        }

        if (!sserver::common::net::HasValidMessageMagic(frame.header) ||
            frame.header.message_type != static_cast<std::uint16_t>(sserver::common::net::MessageType::kAvStream)) {
            sserver::common::log::Logger::Error("integration transport test received invalid stream header");
            close(socket_fd);
            bootstrap.Stop();
            return 8;
        }
        if (frame.metadata.capture_timestamp_ns == 0 || frame.payload.empty() || !PayloadHasExpectedPrefix(frame.payload)) {
            sserver::common::log::Logger::Error("integration transport test received invalid payload or metadata");
            close(socket_fd);
            bootstrap.Stop();
            return 9;
        }
        if (frame.metadata.transport_send_timestamp_ns == 0 ||
            frame.metadata.encode_end_timestamp_ns < frame.metadata.encode_start_timestamp_ns ||
            frame.metadata.encode_start_timestamp_ns < frame.metadata.capture_timestamp_ns) {
            sserver::common::log::Logger::Error("integration transport test received inconsistent timing metadata");
            close(socket_fd);
            bootstrap.Stop();
            return 11;
        }
        if (frame_index > 0 && frame.metadata.sequence <= previous_sequence) {
            sserver::common::log::Logger::Error("integration transport test detected non-monotonic frame sequence");
            close(socket_fd);
            bootstrap.Stop();
            return 10;
        }

        previous_sequence = frame.metadata.sequence;
    }

    if (config.transport.backend == "udp" && !nack_recovery_exercised) {
        if (!config.transport.udp_enable_fec || config.transport.udp_enable_nack) {
            sserver::common::log::Logger::Error("integration transport test did not exercise expected UDP NACK recovery");
            close(socket_fd);
            bootstrap.Stop();
            return 12;
        }
    }
    if (config.transport.backend == "udp" &&
        config.transport.udp_enable_fec &&
        config.transport.udp_enable_nack &&
        !nack_recovery_exercised) {
        sserver::common::log::Logger::Error(
                "integration transport test did not exercise expected UDP NACK recovery with FEC enabled");
        close(socket_fd);
        bootstrap.Stop();
        return 14;
    }
    if (config.transport.backend == "udp" &&
        config.transport.udp_enable_fec &&
        !config.transport.udp_enable_nack &&
        !fec_recovery_exercised) {
        sserver::common::log::Logger::Error("integration transport test did not exercise expected UDP FEC recovery");
        close(socket_fd);
        bootstrap.Stop();
        return 13;
    }

    close(socket_fd);
    bootstrap.Stop();
    sserver::common::log::Logger::Info("integration transport test completed successfully");
    return 0;
}
