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

bool LoadConfig(const std::string &relative_path, sserver::config::AppConfig *config) {
    std::string error_message;
    const std::string config_path = std::string(SSERVER_SOURCE_DIR) + "/" + relative_path;
    if (!sserver::config::ConfigLoader::LoadFromFile(config_path, config, &error_message)) {
        std::cerr << "failed to load " << relative_path << ": " << error_message << std::endl;
        return false;
    }
    if (!config->Validate(&error_message)) {
        std::cerr << "failed to validate " << relative_path << ": " << error_message << std::endl;
        return false;
    }
    return true;
}

bool TestBalancedProfileMatchesExpectedDefaults() {
    sserver::config::AppConfig config;
    if (!LoadConfig("config/sclient_udp.conf", &config)) {
        return false;
    }

    return Expect(config.transport.backend == "udp", "expected balanced profile to use udp backend") &&
           Expect(config.transport.listen_port == 19100, "expected balanced profile to listen on 19100") &&
           Expect(
                   config.transport.udp_target_payload_size == 65000,
                   "expected balanced profile udp_target_payload_size=65000") &&
           Expect(!config.transport.udp_enable_nack, "expected balanced profile to disable NACK") &&
           Expect(!config.transport.udp_enable_fec, "expected balanced profile to keep FEC disabled");
}

bool TestAdaptiveProfileEnablesFecWithLargerJitterTolerance() {
    sserver::config::AppConfig config;
    if (!LoadConfig("config/sclient_udp_adaptive.conf", &config)) {
        return false;
    }

    return Expect(config.transport.backend == "udp", "expected adaptive profile to use udp backend") &&
           Expect(config.transport.listen_port == 19101, "expected adaptive profile to listen on 19101") &&
           Expect(
                   config.transport.udp_target_payload_size == 1000,
                   "expected adaptive profile udp_target_payload_size=1000") &&
           Expect(config.transport.udp_enable_nack, "expected adaptive profile to enable NACK") &&
           Expect(config.transport.udp_enable_fec, "expected adaptive profile to enable FEC");
}

bool TestResilientProfileEnablesFecAndRetransmitCache() {
    sserver::config::AppConfig config;
    if (!LoadConfig("config/sclient_udp_resilient.conf", &config)) {
        return false;
    }

    return Expect(config.transport.backend == "udp", "expected resilient profile to use udp backend") &&
           Expect(config.transport.listen_port == 19102, "expected resilient profile to listen on 19102") &&
           Expect(
                   config.transport.udp_target_payload_size == 256,
                   "expected resilient profile udp_target_payload_size=256") &&
           Expect(config.transport.udp_enable_nack, "expected resilient profile to enable NACK") &&
           Expect(config.transport.udp_enable_fec, "expected resilient profile to enable FEC") &&
           Expect(
                   config.transport.udp_retransmit_cache_frames == 32,
                   "expected resilient profile udp_retransmit_cache_frames=32") &&
           Expect(
                   config.transport.udp_retransmit_cache_max_age_ms == 500,
                   "expected resilient profile udp_retransmit_cache_max_age_ms=500") &&
           Expect(
                   config.transport.udp_retransmit_max_fragments_per_request == 16,
                   "expected resilient profile udp_retransmit_max_fragments_per_request=16");
}

}  // namespace

int main() {
    if (!TestBalancedProfileMatchesExpectedDefaults()) {
        return EXIT_FAILURE;
    }
    if (!TestAdaptiveProfileEnablesFecWithLargerJitterTolerance()) {
        return EXIT_FAILURE;
    }
    if (!TestResilientProfileEnablesFecAndRetransmitCache()) {
        return EXIT_FAILURE;
    }
    return EXIT_SUCCESS;
}
