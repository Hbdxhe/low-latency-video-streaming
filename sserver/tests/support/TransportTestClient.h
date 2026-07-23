#ifndef SSERVER_TESTS_SUPPORT_TRANSPORTTESTCLIENT_H
#define SSERVER_TESTS_SUPPORT_TRANSPORTTESTCLIENT_H

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include <chrono>
#include <cstdint>
#include <cstring>
#include <map>
#include <string>
#include <thread>
#include <vector>

#include "app/AppBootstrap.h"
#include "common/net/StreamProtocol.h"
#include "common/time/MonotonicClock.h"
#include "config/AppConfig.h"

namespace sserver {
namespace tests {
namespace support {

struct ReceivedFrame {
    common::net::MessageHeader header;
    common::net::FrameDiagnosticMetadata metadata;
    std::uint64_t receive_timestamp_ns = 0;
    std::vector<std::uint8_t> payload;
};

inline bool ReceiveAll(int socket_fd, char *buffer, std::size_t length) {
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

inline bool SendKeepAlive(int socket_fd) {
    common::net::MessageHeader header{};
    common::net::FillMessageMagic(header.head_id);
    header.message_type = static_cast<std::uint16_t>(common::net::MessageType::kKeepAlive);
    common::net::UdpReceiverReport report{};
    report.report_timestamp_ns = common::time::MonotonicNowNs();
    header.payload_length = static_cast<std::uint32_t>(sizeof(report));

    char payload[sizeof(header) + sizeof(report)];
    std::memcpy(payload, &header, sizeof(header));
    std::memcpy(payload + sizeof(header), &report, sizeof(report));
    return send(socket_fd, payload, sizeof(payload), 0) == static_cast<ssize_t>(sizeof(payload));
}

inline bool SendUdpNack(int socket_fd, const std::vector<common::net::UdpNackItem> &items) {
    if (items.empty()) {
        return true;
    }

    common::net::MessageHeader header{};
    common::net::FillMessageMagic(header.head_id);
    header.message_type = static_cast<std::uint16_t>(common::net::MessageType::kUdpNack);
    header.payload_length = static_cast<std::uint32_t>(
            sizeof(common::net::UdpNackHeader) + items.size() * sizeof(common::net::UdpNackItem));

    common::net::UdpNackHeader nack_header{};
    nack_header.request_timestamp_ns = common::time::MonotonicNowNs();
    nack_header.request_count = static_cast<std::uint16_t>(items.size());

    std::vector<char> payload(sizeof(header) + header.payload_length);
    std::memcpy(payload.data(), &header, sizeof(header));
    std::memcpy(payload.data() + sizeof(header), &nack_header, sizeof(nack_header));
    std::memcpy(
            payload.data() + sizeof(header) + sizeof(nack_header),
            items.data(),
            items.size() * sizeof(common::net::UdpNackItem));
    return send(socket_fd, payload.data(), payload.size(), 0) == static_cast<ssize_t>(payload.size());
}

inline bool WaitForBoundPort(const app::AppBootstrap &bootstrap, int *port) {
    if (port == nullptr) {
        return false;
    }

    *port = 0;
    for (int attempt = 0; attempt < 50 && *port == 0; ++attempt) {
        *port = bootstrap.bound_port();
        if (*port == 0) {
            std::this_thread::sleep_for(std::chrono::milliseconds(20));
        }
    }
    return *port != 0;
}

inline bool ConnectClient(const config::TransportConfig &transport_config, int port, int *socket_fd) {
    if (socket_fd == nullptr) {
        return false;
    }

    *socket_fd = socket(AF_INET, transport_config.backend == "udp" ? SOCK_DGRAM : SOCK_STREAM, 0);
    if (*socket_fd < 0) {
        return false;
    }

    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_port = htons(static_cast<std::uint16_t>(port));
    address.sin_addr.s_addr = inet_addr(transport_config.bind_address.c_str());

    if (connect(*socket_fd, reinterpret_cast<sockaddr *>(&address), sizeof(address)) != 0) {
        close(*socket_fd);
        *socket_fd = -1;
        return false;
    }

    if (transport_config.backend == "udp") {
        setsockopt(
                *socket_fd,
                SOL_SOCKET,
                SO_RCVBUF,
                &transport_config.udp_receive_buffer_bytes,
                sizeof(transport_config.udp_receive_buffer_bytes));
        if (!SendKeepAlive(*socket_fd)) {
            close(*socket_fd);
            *socket_fd = -1;
            return false;
        }
    }

    return true;
}

inline bool ReceiveTcpFrame(int socket_fd, bool expect_metadata, ReceivedFrame *frame) {
    if (frame == nullptr) {
        return false;
    }

    if (!ReceiveAll(socket_fd, reinterpret_cast<char *>(&frame->header), sizeof(frame->header))) {
        return false;
    }
    if (!common::net::HasValidMessageMagic(frame->header)) {
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
    frame->receive_timestamp_ns = common::time::MonotonicNowNs();

    return true;
}

struct UdpFrameAssemblyState {
    common::net::MessageHeader header;
    common::net::FrameDiagnosticMetadata metadata;
    std::vector<std::uint8_t> payload;
    std::vector<bool> received_fragments;
    std::vector<std::uint32_t> fragment_offsets;
    std::vector<std::uint32_t> fragment_payload_sizes;
    std::size_t received_fragment_count = 0;
    std::vector<std::uint8_t> fec_payload;
    bool has_fec_payload = false;
};

inline bool MaybeRecoverUdpAssemblyWithFec(UdpFrameAssemblyState *assembly) {
    if (assembly == nullptr ||
        !assembly->has_fec_payload ||
        assembly->received_fragments.empty() ||
        assembly->received_fragment_count >= assembly->received_fragments.size()) {
        return false;
    }

    std::size_t missing_index = assembly->received_fragments.size();
    std::size_t missing_count = 0;
    for (std::size_t index = 0; index < assembly->received_fragments.size(); ++index) {
        if (!assembly->received_fragments[index]) {
            missing_index = index;
            ++missing_count;
            if (missing_count > 1) {
                return false;
            }
        }
    }
    if (missing_count != 1) {
        return false;
    }

    std::uint32_t missing_offset = 0;
    if (missing_index > 0) {
        missing_offset = assembly->fragment_offsets[missing_index - 1] +
                assembly->fragment_payload_sizes[missing_index - 1];
    }

    std::uint32_t missing_size = 0;
    if (missing_index + 1 < assembly->received_fragments.size()) {
        const std::uint32_t next_offset = assembly->fragment_offsets[missing_index + 1];
        if (next_offset < missing_offset) {
            return false;
        }
        missing_size = next_offset - missing_offset;
    } else if (assembly->payload.size() >= missing_offset) {
        missing_size = static_cast<std::uint32_t>(assembly->payload.size() - missing_offset);
    }

    if (missing_size == 0 ||
        missing_size > assembly->fec_payload.size() ||
        static_cast<std::size_t>(missing_offset) + missing_size > assembly->payload.size()) {
        return false;
    }

    std::vector<std::uint8_t> recovered = assembly->fec_payload;
    for (std::size_t index = 0; index < assembly->received_fragments.size(); ++index) {
        if (!assembly->received_fragments[index]) {
            continue;
        }
        const std::uint32_t fragment_offset = assembly->fragment_offsets[index];
        const std::uint32_t fragment_size = assembly->fragment_payload_sizes[index];
        if (static_cast<std::size_t>(fragment_offset) + fragment_size > assembly->payload.size()) {
            return false;
        }
        for (std::size_t byte_index = 0; byte_index < fragment_size; ++byte_index) {
            recovered[byte_index] ^= assembly->payload[fragment_offset + byte_index];
        }
    }

    std::memcpy(
            assembly->payload.data() + missing_offset,
            recovered.data(),
            missing_size);
    assembly->received_fragments[missing_index] = true;
    assembly->fragment_offsets[missing_index] = missing_offset;
    assembly->fragment_payload_sizes[missing_index] = missing_size;
    ++assembly->received_fragment_count;
    return true;
}

inline bool TryFinalizeUdpAssembly(
        std::uint64_t frame_sequence,
        std::map<std::uint64_t, UdpFrameAssemblyState> *assemblies,
        ReceivedFrame *frame) {
    if (assemblies == nullptr || frame == nullptr) {
        return false;
    }

    std::map<std::uint64_t, UdpFrameAssemblyState>::iterator it = assemblies->find(frame_sequence);
    if (it == assemblies->end()) {
        return false;
    }
    UdpFrameAssemblyState &assembly = it->second;
    if (assembly.received_fragments.empty() || assembly.received_fragment_count != assembly.received_fragments.size()) {
        return false;
    }

    frame->header = assembly.header;
    frame->metadata = assembly.metadata;
    frame->receive_timestamp_ns = common::time::MonotonicNowNs();
    frame->payload.swap(assembly.payload);
    assemblies->erase(it);
    return true;
}

inline bool ConsumeUdpDatagram(
        int socket_fd,
        const std::vector<char> &datagram,
        std::size_t datagram_size,
        bool expect_metadata,
        std::map<std::uint64_t, UdpFrameAssemblyState> *assemblies,
        ReceivedFrame *frame,
        bool drop_first_data_fragment,
        bool send_nack_for_dropped_fragment,
        bool *dropped_data_fragment,
        bool *recovery_exercised) {
    if (assemblies == nullptr || frame == nullptr) {
        return false;
    }

    if (datagram_size < sizeof(frame->header) + sizeof(common::net::UdpFrameFragmentHeader)) {
        return false;
    }

    common::net::MessageHeader header{};
    std::memcpy(&header, datagram.data(), sizeof(header));
    if (!common::net::HasValidMessageMagic(header) ||
        header.message_type != static_cast<std::uint16_t>(common::net::MessageType::kAvStream) ||
        header.payload_length < sizeof(common::net::UdpFrameFragmentHeader)) {
        return false;
    }

    common::net::UdpFrameFragmentHeader fragment_header{};
    std::memcpy(&fragment_header, datagram.data() + sizeof(header), sizeof(fragment_header));
    if (fragment_header.fragment_count == 0 ||
        fragment_header.fragment_role > static_cast<std::uint16_t>(common::net::UdpFragmentRole::kXorParity) ||
        (fragment_header.fragment_role == static_cast<std::uint16_t>(common::net::UdpFragmentRole::kData) &&
         fragment_header.fragment_index >= fragment_header.fragment_count)) {
        return false;
    }

    const std::size_t fragment_payload_size = datagram_size - sizeof(header) - sizeof(fragment_header);
    if (fragment_payload_size + sizeof(fragment_header) != header.payload_length) {
        return false;
    }
    if (fragment_header.fragment_role == static_cast<std::uint16_t>(common::net::UdpFragmentRole::kData) &&
        fragment_header.fragment_offset + fragment_payload_size > fragment_header.frame_payload_size) {
        return false;
    }

    if (fragment_header.fragment_role == static_cast<std::uint16_t>(common::net::UdpFragmentRole::kData) &&
        drop_first_data_fragment &&
        dropped_data_fragment != nullptr &&
        !*dropped_data_fragment) {
        if (send_nack_for_dropped_fragment) {
            common::net::UdpNackItem nack_item{};
            nack_item.frame_sequence = fragment_header.frame_sequence;
            nack_item.fragment_index = fragment_header.fragment_index;
            if (!SendUdpNack(socket_fd, std::vector<common::net::UdpNackItem>(1, nack_item))) {
                return false;
            }
        }
        *dropped_data_fragment = true;
        if (recovery_exercised != nullptr) {
            *recovery_exercised = true;
        }
        return false;
    }

    UdpFrameAssemblyState &assembly = (*assemblies)[fragment_header.frame_sequence];
    if (assembly.received_fragments.empty()) {
        assembly.header = header;
        assembly.metadata.sequence = fragment_header.frame_sequence;
        assembly.metadata.capture_timestamp_ns = expect_metadata ? fragment_header.capture_timestamp_ns : 0;
        assembly.metadata.encode_start_timestamp_ns = expect_metadata ? fragment_header.encode_start_timestamp_ns : 0;
        assembly.metadata.encode_end_timestamp_ns = expect_metadata ? fragment_header.encode_end_timestamp_ns : 0;
        assembly.metadata.transport_send_timestamp_ns = expect_metadata ? fragment_header.transport_send_timestamp_ns : 0;
        assembly.payload.resize(fragment_header.frame_payload_size);
        assembly.received_fragments.assign(fragment_header.fragment_count, false);
        assembly.fragment_offsets.assign(fragment_header.fragment_count, 0);
        assembly.fragment_payload_sizes.assign(fragment_header.fragment_count, 0);
    }

    if (assembly.received_fragments.size() != fragment_header.fragment_count ||
        assembly.payload.size() != fragment_header.frame_payload_size) {
        assemblies->erase(fragment_header.frame_sequence);
        return false;
    }

    if (fragment_header.fragment_role == static_cast<std::uint16_t>(common::net::UdpFragmentRole::kXorParity)) {
        assembly.fec_payload.assign(
                reinterpret_cast<const std::uint8_t *>(datagram.data() + sizeof(header) + sizeof(fragment_header)),
                reinterpret_cast<const std::uint8_t *>(datagram.data() + datagram_size));
        assembly.has_fec_payload = true;
    } else if (!assembly.received_fragments[fragment_header.fragment_index]) {
        std::memcpy(
                assembly.payload.data() + fragment_header.fragment_offset,
                datagram.data() + sizeof(header) + sizeof(fragment_header),
                fragment_payload_size);
        assembly.received_fragments[fragment_header.fragment_index] = true;
        assembly.fragment_offsets[fragment_header.fragment_index] = fragment_header.fragment_offset;
        assembly.fragment_payload_sizes[fragment_header.fragment_index] =
                static_cast<std::uint32_t>(fragment_payload_size);
        ++assembly.received_fragment_count;
    }

    if (assembly.received_fragment_count < assembly.received_fragments.size()) {
        MaybeRecoverUdpAssemblyWithFec(&assembly);
    }
    return TryFinalizeUdpAssembly(fragment_header.frame_sequence, assemblies, frame);
}

inline bool ReceiveUdpFrame(int socket_fd, std::size_t max_datagram_size, bool expect_metadata, ReceivedFrame *frame) {
    if (frame == nullptr) {
        return false;
    }

    std::map<std::uint64_t, UdpFrameAssemblyState> assemblies;

    while (true) {
        std::vector<char> datagram(max_datagram_size);
        const ssize_t received = recv(socket_fd, datagram.data(), datagram.size(), 0);
        if (received <= 0) {
            return false;
        }
        if (ConsumeUdpDatagram(
                    socket_fd,
                    datagram,
                    static_cast<std::size_t>(received),
                    expect_metadata,
                    &assemblies,
                    frame,
                    false,
                    false,
                    nullptr,
                    nullptr)) {
            return true;
        }
    }
}

inline bool ReceiveUdpFrameWithNackRecovery(
        int socket_fd,
        std::size_t max_datagram_size,
        bool expect_metadata,
        ReceivedFrame *frame,
        bool *nack_sent) {
    if (frame == nullptr) {
        return false;
    }

    if (nack_sent != nullptr) {
        *nack_sent = false;
    }

    std::map<std::uint64_t, UdpFrameAssemblyState> assemblies;
    bool dropped_fragment = false;

    while (true) {
        std::vector<char> datagram(max_datagram_size);
        const ssize_t received = recv(socket_fd, datagram.data(), datagram.size(), 0);
        if (received <= 0) {
            return false;
        }
        if (ConsumeUdpDatagram(
                    socket_fd,
                    datagram,
                    static_cast<std::size_t>(received),
                    expect_metadata,
                    &assemblies,
                    frame,
                    true,
                    true,
                    &dropped_fragment,
                    nack_sent)) {
            return true;
        }
    }
}

inline bool ReceiveUdpFrameWithFecRecovery(
        int socket_fd,
        std::size_t max_datagram_size,
        bool expect_metadata,
        ReceivedFrame *frame,
        bool *fec_recovery_exercised) {
    if (frame == nullptr) {
        return false;
    }

    if (fec_recovery_exercised != nullptr) {
        *fec_recovery_exercised = false;
    }

    std::map<std::uint64_t, UdpFrameAssemblyState> assemblies;
    bool dropped_fragment = false;

    while (true) {
        std::vector<char> datagram(max_datagram_size);
        const ssize_t received = recv(socket_fd, datagram.data(), datagram.size(), 0);
        if (received <= 0) {
            return false;
        }

        if (ConsumeUdpDatagram(
                    socket_fd,
                    datagram,
                    static_cast<std::size_t>(received),
                    expect_metadata,
                    &assemblies,
                    frame,
                    true,
                    false,
                    &dropped_fragment,
                    fec_recovery_exercised)) {
            return true;
        }
    }
}

inline bool ReceiveFrame(int socket_fd, const config::TransportConfig &transport_config, ReceivedFrame *frame) {
    if (transport_config.backend == "udp") {
        return ReceiveUdpFrame(
                socket_fd,
                transport_config.udp_max_datagram_size,
                transport_config.embed_frame_metadata,
                frame);
    }
    return ReceiveTcpFrame(socket_fd, transport_config.embed_frame_metadata, frame);
}

inline bool RefreshClientKeepAlive(int socket_fd, const config::TransportConfig &transport_config, int frame_index) {
    if (transport_config.backend == "udp" && frame_index % 15 == 0) {
        return SendKeepAlive(socket_fd);
    }
    return true;
}

}  // namespace support
}  // namespace tests
}  // namespace sserver

#endif  // SSERVER_TESTS_SUPPORT_TRANSPORTTESTCLIENT_H
