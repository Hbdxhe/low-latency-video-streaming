#include <cstdint>
#include <vector>

#include "common/net/RtpProtocol.h"
#include "tests/support/TestAssertions.h"

namespace {

using sclient::tests::support::Expect;

bool TestParsesLatencyExtension() {
    const sclient::common::net::RtpHeaderFields header_fields = {
            true,
            96,
            321,
            654321,
            0x13572468,
    };
    const sclient::common::net::RtpLatencyExtension latency_extension = {
            123456789ULL,
            123556789ULL,
    };
    const sclient::common::net::RtpHeaderExtension header_extension =
            sclient::common::net::BuildRtpLatencyHeaderExtension(latency_extension);

    std::vector<std::uint8_t> packet;
    if (!sclient::common::net::WriteRtpHeader(header_fields, &header_extension, &packet)) {
        return Expect(false, "failed to serialize RTP header with latency extension");
    }

    const std::vector<std::uint8_t> payload = {0x65, 0x11, 0x22, 0x33};
    packet.insert(packet.end(), payload.begin(), payload.end());

    sclient::common::net::RtpHeaderFields parsed_header_fields;
    sclient::common::net::RtpHeaderExtension parsed_extension;
    std::size_t header_size = 0;
    if (!sclient::common::net::ParseRtpHeader(
                packet.data(),
                packet.size(),
                &parsed_header_fields,
                &header_size,
                &parsed_extension)) {
        return Expect(false, "failed to parse RTP header with latency extension");
    }

    sclient::common::net::RtpLatencyExtension parsed_latency_extension;
    if (!sclient::common::net::ParseRtpLatencyExtension(parsed_extension, &parsed_latency_extension)) {
        return Expect(false, "failed to parse RTP latency extension payload");
    }

    return Expect(header_size == sclient::common::net::kRtpHeaderSize +
                                      sclient::common::net::kRtpExtensionHeaderSize +
                                      sclient::common::net::kRtpLatencyExtensionPayloadSize,
                  "unexpected RTP header size with latency extension") &&
           Expect(parsed_header_fields.marker == header_fields.marker, "marker bit mismatch") &&
           Expect(parsed_header_fields.sequence_number == header_fields.sequence_number, "sequence number mismatch") &&
           Expect(parsed_header_fields.timestamp == header_fields.timestamp, "timestamp mismatch") &&
           Expect(parsed_header_fields.ssrc == header_fields.ssrc, "ssrc mismatch") &&
           Expect(parsed_extension.profile_id == sclient::common::net::kRtpLatencyExtensionProfileId,
                  "unexpected RTP extension profile id") &&
           Expect(parsed_extension.payload.size() == sclient::common::net::kRtpLatencyExtensionPayloadSize,
                  "unexpected RTP extension payload size") &&
           Expect(parsed_latency_extension.capture_timestamp_ns == latency_extension.capture_timestamp_ns,
                  "capture timestamp mismatch after RTP extension parse") &&
           Expect(parsed_latency_extension.transport_send_timestamp_ns == latency_extension.transport_send_timestamp_ns,
                  "send timestamp mismatch after RTP extension parse");
}

bool TestRejectsInvalidLatencyExtension() {
    const sclient::common::net::RtpHeaderFields header_fields = {
            false,
            96,
            322,
            654322,
            0x24681357,
    };

    sclient::common::net::RtpHeaderExtension invalid_extension;
    invalid_extension.profile_id = 0x1111;
    invalid_extension.payload.assign(sclient::common::net::kRtpLatencyExtensionPayloadSize, 0xAA);

    std::vector<std::uint8_t> packet;
    if (!sclient::common::net::WriteRtpHeader(header_fields, &invalid_extension, &packet)) {
        return Expect(false, "failed to serialize RTP header with invalid extension");
    }
    packet.push_back(0x41);

    sclient::common::net::RtpHeaderFields parsed_header_fields;
    sclient::common::net::RtpHeaderExtension parsed_extension;
    std::size_t header_size = 0;
    if (!sclient::common::net::ParseRtpHeader(
                packet.data(),
                packet.size(),
                &parsed_header_fields,
                &header_size,
                &parsed_extension)) {
        return Expect(false, "failed to parse RTP header with invalid extension");
    }

    sclient::common::net::RtpLatencyExtension parsed_latency_extension;
    return Expect(header_size == sclient::common::net::kRtpHeaderSize +
                                      sclient::common::net::kRtpExtensionHeaderSize +
                                      sclient::common::net::kRtpLatencyExtensionPayloadSize,
                  "unexpected header size for invalid RTP extension packet") &&
           Expect(parsed_header_fields.sequence_number == header_fields.sequence_number, "sequence number mismatch") &&
           Expect(parsed_extension.profile_id == invalid_extension.profile_id, "invalid profile id not preserved") &&
           Expect(!sclient::common::net::ParseRtpLatencyExtension(parsed_extension, &parsed_latency_extension),
                  "expected invalid RTP extension payload to be rejected");
}

}  // namespace

int main() {
    if (!TestParsesLatencyExtension()) {
        return 1;
    }
    if (!TestRejectsInvalidLatencyExtension()) {
        return 1;
    }
    return 0;
}
