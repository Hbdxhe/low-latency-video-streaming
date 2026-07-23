#include <cstdlib>
#include <iostream>
#include <string>

#include "common/net/RtpProtocol.h"
#include "config/AppConfig.h"

namespace {

bool Expect(bool condition, const std::string &message) {
    if (!condition) {
        std::cerr << message << std::endl;
        return false;
    }
    return true;
}

bool TestAcceptsRtpPayloadThatFitsWithinUdpDatagram() {
    sserver::config::AppConfig config = sserver::config::AppConfig::CreateDefault();
    config.app_name = "rtp-config-test";
    config.capture.enabled = false;
    config.transport.enabled = true;
    config.transport.backend = "rtp";
    config.transport.udp_max_datagram_size = 512;
    config.transport.udp_target_payload_size = 256;
    config.transport.rtp_max_payload_size =
            512 - sserver::common::net::kRtpPacketOverheadWithLatencyExtension;

    std::string error_message;
    return Expect(config.Validate(&error_message), "expected RTP payload size at UDP boundary to validate");
}

bool TestRejectsRtpPayloadThatIgnoresHeaderOverhead() {
    sserver::config::AppConfig config = sserver::config::AppConfig::CreateDefault();
    config.app_name = "rtp-config-test";
    config.capture.enabled = false;
    config.transport.enabled = true;
    config.transport.backend = "rtp";
    config.transport.udp_max_datagram_size = 512;
    config.transport.udp_target_payload_size = 256;
    config.transport.rtp_max_payload_size =
            513 - sserver::common::net::kRtpPacketOverheadWithLatencyExtension;

    std::string error_message;
    if (!Expect(!config.Validate(&error_message), "expected oversized RTP payload size to be rejected")) {
        return false;
    }
    return Expect(
            error_message.find("RTP header overhead") != std::string::npos,
            "expected validation error to mention RTP header overhead");
}

bool TestAllowsDisablingRtpLatencyExtension() {
    sserver::config::AppConfig config = sserver::config::AppConfig::CreateDefault();
    config.app_name = "rtp-config-test";
    config.capture.enabled = false;
    config.transport.enabled = true;
    config.transport.backend = "rtp";
    config.transport.rtp_enable_latency_extension = false;

    std::string error_message;
    return Expect(config.Validate(&error_message), "expected RTP config without latency extension to validate");
}

}  // namespace

int main() {
    if (!TestAcceptsRtpPayloadThatFitsWithinUdpDatagram()) {
        return EXIT_FAILURE;
    }
    if (!TestRejectsRtpPayloadThatIgnoresHeaderOverhead()) {
        return EXIT_FAILURE;
    }
    if (!TestAllowsDisablingRtpLatencyExtension()) {
        return EXIT_FAILURE;
    }
    return EXIT_SUCCESS;
}
