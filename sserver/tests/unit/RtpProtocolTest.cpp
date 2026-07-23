#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <string>
#include <vector>

#include "common/net/RtpProtocol.h"

namespace {

bool Expect(bool condition, const std::string &message) {
    if (!condition) {
        std::cerr << message << std::endl;
        return false;
    }
    return true;
}

bool TestParsePlainRtpHeaderWithoutExtension() {
    sserver::common::net::RtpHeaderFields written_header;
    written_header.marker = true;
    written_header.payload_type = 96;
    written_header.sequence_number = 42;
    written_header.timestamp = 123456789u;
    written_header.ssrc = 0x11223344u;

    std::vector<std::uint8_t> packet;
    sserver::common::net::WriteRtpHeader(written_header, &packet);
    packet.push_back(0xAA);
    packet.push_back(0xBB);

    sserver::common::net::RtpHeaderFields parsed_header;
    sserver::common::net::RtpHeaderExtension parsed_extension;
    std::size_t header_size = 0;
    if (!Expect(
                sserver::common::net::ParseRtpHeader(
                        packet.data(),
                        packet.size(),
                        &parsed_header,
                        &header_size,
                        &parsed_extension),
                "failed to parse plain RTP header")) {
        return false;
    }

    return Expect(header_size == sserver::common::net::kRtpHeaderSize, "unexpected RTP header size") &&
           Expect(parsed_header.marker == written_header.marker, "marker bit mismatch") &&
           Expect(parsed_header.payload_type == written_header.payload_type, "payload type mismatch") &&
           Expect(parsed_header.sequence_number == written_header.sequence_number, "sequence number mismatch") &&
           Expect(parsed_header.timestamp == written_header.timestamp, "timestamp mismatch") &&
           Expect(parsed_header.ssrc == written_header.ssrc, "ssrc mismatch") &&
           Expect(parsed_extension.profile_id == 0, "unexpected extension profile for plain RTP header") &&
           Expect(parsed_extension.payload.empty(), "unexpected extension payload for plain RTP header");
}

bool TestLatencyExtensionEncodeDecodeRoundTrip() {
    sserver::common::net::RtpHeaderFields written_header;
    written_header.marker = false;
    written_header.payload_type = 97;
    written_header.sequence_number = 4096;
    written_header.timestamp = 0x89ABCDEFu;
    written_header.ssrc = 0x55667788u;

    const sserver::common::net::RtpLatencyExtension written_latency_extension {
            0x0102030405060708ULL,
            0x1112131415161718ULL};
    const sserver::common::net::RtpHeaderExtension written_extension =
            sserver::common::net::BuildRtpLatencyHeaderExtension(written_latency_extension);

    std::vector<std::uint8_t> packet;
    if (!Expect(
                sserver::common::net::WriteRtpHeader(written_header, &written_extension, &packet),
                "failed to serialize RTP header with latency extension")) {
        return false;
    }
    packet.push_back(0x7C);
    packet.push_back(0x85);

    sserver::common::net::RtpHeaderFields parsed_header;
    sserver::common::net::RtpHeaderExtension parsed_extension;
    std::size_t header_size = 0;
    if (!Expect(
                sserver::common::net::ParseRtpHeader(
                        packet.data(),
                        packet.size(),
                        &parsed_header,
                        &header_size,
                        &parsed_extension),
                "failed to parse RTP header with latency extension")) {
        return false;
    }

    sserver::common::net::RtpLatencyExtension parsed_latency_extension;
    if (!Expect(
                sserver::common::net::ParseRtpLatencyExtension(parsed_extension, &parsed_latency_extension),
                "failed to parse RTP latency extension payload")) {
        return false;
    }

    return Expect(
                   header_size ==
                           sserver::common::net::kRtpHeaderSize +
                                   sserver::common::net::kRtpExtensionHeaderSize +
                                   sserver::common::net::kRtpLatencyExtensionPayloadSize,
                   "unexpected RTP header size with extension") &&
           Expect(parsed_header.payload_type == written_header.payload_type, "payload type mismatch with extension") &&
           Expect(
                   parsed_extension.profile_id == sserver::common::net::kRtpLatencyExtensionProfileId,
                   "unexpected latency extension profile id") &&
           Expect(
                   parsed_extension.payload.size() == sserver::common::net::kRtpLatencyExtensionPayloadSize,
                   "unexpected latency extension payload size") &&
           Expect(
                   parsed_latency_extension.capture_timestamp_ns ==
                           written_latency_extension.capture_timestamp_ns,
                   "capture timestamp mismatch after latency extension round trip") &&
           Expect(
                   parsed_latency_extension.transport_send_timestamp_ns ==
                           written_latency_extension.transport_send_timestamp_ns,
                   "send timestamp mismatch after latency extension round trip") &&
           Expect(packet[header_size] == 0x7C && packet[header_size + 1] == 0x85, "payload offset changed unexpectedly");
}

bool TestTransportSendTimestampUpdateInPlace() {
    sserver::common::net::RtpHeaderFields header;
    header.payload_type = 96;
    header.sequence_number = 7;
    header.timestamp = 9000;
    header.ssrc = 0xCAFEBABEu;

    const sserver::common::net::RtpLatencyExtension original_extension {123456789ULL, 0ULL};
    const sserver::common::net::RtpHeaderExtension header_extension =
            sserver::common::net::BuildRtpLatencyHeaderExtension(original_extension);

    std::vector<std::uint8_t> packet;
    if (!Expect(
                sserver::common::net::WriteRtpHeader(header, &header_extension, &packet),
                "failed to write RTP header for in-place timestamp update")) {
        return false;
    }
    packet.push_back(0x41);

    const std::uint64_t updated_send_timestamp_ns = 987654321ULL;
    if (!Expect(
                sserver::common::net::UpdateRtpLatencyExtensionTransportSendTimestamp(
                        packet.data(),
                        packet.size(),
                        updated_send_timestamp_ns),
                "failed to update RTP latency extension send timestamp in place")) {
        return false;
    }

    sserver::common::net::RtpHeaderFields parsed_header;
    sserver::common::net::RtpHeaderExtension parsed_extension;
    std::size_t header_size = 0;
    if (!Expect(
                sserver::common::net::ParseRtpHeader(
                        packet.data(),
                        packet.size(),
                        &parsed_header,
                        &header_size,
                        &parsed_extension),
                "failed to parse RTP header after in-place timestamp update")) {
        return false;
    }

    sserver::common::net::RtpLatencyExtension parsed_latency_extension;
    if (!Expect(
                sserver::common::net::ParseRtpLatencyExtension(parsed_extension, &parsed_latency_extension),
                "failed to parse RTP latency extension after in-place update")) {
        return false;
    }

    return Expect(parsed_latency_extension.capture_timestamp_ns == original_extension.capture_timestamp_ns,
                  "capture timestamp changed during in-place send timestamp update") &&
           Expect(parsed_latency_extension.transport_send_timestamp_ns == updated_send_timestamp_ns,
                  "send timestamp was not updated in place") &&
           Expect(packet[header_size] == 0x41, "payload changed during in-place send timestamp update");
}

}  // namespace

int main() {
    if (!TestParsePlainRtpHeaderWithoutExtension()) {
        return EXIT_FAILURE;
    }
    if (!TestLatencyExtensionEncodeDecodeRoundTrip()) {
        return EXIT_FAILURE;
    }
    if (!TestTransportSendTimestampUpdateInPlace()) {
        return EXIT_FAILURE;
    }
    return EXIT_SUCCESS;
}
