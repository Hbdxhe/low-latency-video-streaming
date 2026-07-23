#include <cstdlib>
#include <iostream>
#include <string>

#include "config/AppConfig.h"

namespace {

bool Expect(bool condition, const std::string &message) {
    if (!condition) {
        std::cerr << message << std::endl;
        return false;
    }
    return true;
}

bool TestAcceptsDefaultUdpTransportConfig() {
    sserver::config::AppConfig config = sserver::config::AppConfig::CreateDefault();
    config.app_name = "udp-config-test";
    config.capture.enabled = false;
    config.transport.enabled = true;
    config.transport.backend = "udp";

    std::string error_message;
    return Expect(config.Validate(&error_message), "expected default UDP transport config to validate");
}

bool TestRejectsUdpPayloadSizeBelowMinimum() {
    sserver::config::AppConfig config = sserver::config::AppConfig::CreateDefault();
    config.app_name = "udp-config-test";
    config.capture.enabled = false;
    config.transport.enabled = true;
    config.transport.backend = "udp";
    config.transport.udp_target_payload_size = 255;

    std::string error_message;
    if (!Expect(!config.Validate(&error_message), "expected undersized UDP payload target to be rejected")) {
        return false;
    }
    return Expect(
            error_message.find("transport.udp_target_payload_size") != std::string::npos,
            "expected validation error to mention transport.udp_target_payload_size");
}

bool TestRejectsZeroUdpRetransmitCacheFrames() {
    sserver::config::AppConfig config = sserver::config::AppConfig::CreateDefault();
    config.app_name = "udp-config-test";
    config.capture.enabled = false;
    config.transport.enabled = true;
    config.transport.backend = "udp";
    config.transport.udp_retransmit_cache_frames = 0;

    std::string error_message;
    if (!Expect(!config.Validate(&error_message), "expected zero UDP retransmit cache frames to be rejected")) {
        return false;
    }
    return Expect(
            error_message.find("transport.udp_retransmit_cache_frames") != std::string::npos,
            "expected validation error to mention transport.udp_retransmit_cache_frames");
}

bool TestRejectsZeroUdpRetransmitMaxFragmentsPerRequest() {
    sserver::config::AppConfig config = sserver::config::AppConfig::CreateDefault();
    config.app_name = "udp-config-test";
    config.capture.enabled = false;
    config.transport.enabled = true;
    config.transport.backend = "udp";
    config.transport.udp_retransmit_max_fragments_per_request = 0;

    std::string error_message;
    if (!Expect(
                !config.Validate(&error_message),
                "expected zero UDP retransmit max fragments per request to be rejected")) {
        return false;
    }
    return Expect(
            error_message.find("transport.udp_retransmit_max_fragments_per_request") != std::string::npos,
            "expected validation error to mention transport.udp_retransmit_max_fragments_per_request");
}

}  // namespace

int main() {
    if (!TestAcceptsDefaultUdpTransportConfig()) {
        return EXIT_FAILURE;
    }
    if (!TestRejectsUdpPayloadSizeBelowMinimum()) {
        return EXIT_FAILURE;
    }
    if (!TestRejectsZeroUdpRetransmitCacheFrames()) {
        return EXIT_FAILURE;
    }
    if (!TestRejectsZeroUdpRetransmitMaxFragmentsPerRequest()) {
        return EXIT_FAILURE;
    }
    return EXIT_SUCCESS;
}
