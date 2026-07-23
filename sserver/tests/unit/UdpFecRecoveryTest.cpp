#include <algorithm>
#include <cstdlib>
#include <iostream>
#include <string>
#include <vector>

#include "tests/support/TransportTestClient.h"

namespace {

bool Expect(bool condition, const std::string &message) {
    if (!condition) {
        std::cerr << message << std::endl;
        return false;
    }
    return true;
}

void XorInto(std::vector<std::uint8_t> *target, const std::vector<std::uint8_t> &source) {
    if (target == nullptr) {
        return;
    }
    if (target->size() < source.size()) {
        target->resize(source.size(), 0);
    }
    for (std::size_t index = 0; index < source.size(); ++index) {
        (*target)[index] ^= source[index];
    }
}

bool TestRecoversSingleMissingFragment() {
    sserver::tests::support::UdpFrameAssemblyState assembly;
    assembly.payload.resize(6, 0);
    assembly.received_fragments = {true, false, true};
    assembly.fragment_offsets = {0, 2, 4};
    assembly.fragment_payload_sizes = {2, 0, 2};
    assembly.received_fragment_count = 2;

    const std::vector<std::uint8_t> fragment0 = {'A', 'B'};
    const std::vector<std::uint8_t> fragment1 = {'C', 'D'};
    const std::vector<std::uint8_t> fragment2 = {'E', 'F'};
    std::copy(fragment0.begin(), fragment0.end(), assembly.payload.begin());
    std::copy(fragment2.begin(), fragment2.end(), assembly.payload.begin() + 4);

    XorInto(&assembly.fec_payload, fragment0);
    XorInto(&assembly.fec_payload, fragment1);
    XorInto(&assembly.fec_payload, fragment2);
    assembly.has_fec_payload = true;

    if (!Expect(
                sserver::tests::support::MaybeRecoverUdpAssemblyWithFec(&assembly),
                "expected FEC recovery to succeed when exactly one fragment is missing")) {
        return false;
    }

    return Expect(
            assembly.received_fragment_count == 3 &&
                    assembly.payload[2] == 'C' &&
                    assembly.payload[3] == 'D',
            "expected missing fragment payload to be reconstructed");
}

bool TestRejectsRecoveryWhenTwoFragmentsAreMissing() {
    sserver::tests::support::UdpFrameAssemblyState assembly;
    assembly.payload.resize(6, 0);
    assembly.received_fragments = {true, false, false};
    assembly.fragment_offsets = {0, 0, 0};
    assembly.fragment_payload_sizes = {2, 0, 0};
    assembly.received_fragment_count = 1;

    const std::vector<std::uint8_t> fragment0 = {'A', 'B'};
    const std::vector<std::uint8_t> fragment1 = {'C', 'D'};
    const std::vector<std::uint8_t> fragment2 = {'E', 'F'};
    std::copy(fragment0.begin(), fragment0.end(), assembly.payload.begin());

    XorInto(&assembly.fec_payload, fragment0);
    XorInto(&assembly.fec_payload, fragment1);
    XorInto(&assembly.fec_payload, fragment2);
    assembly.has_fec_payload = true;

    if (!Expect(
                !sserver::tests::support::MaybeRecoverUdpAssemblyWithFec(&assembly),
                "expected FEC recovery to fail when two fragments are missing")) {
        return false;
    }

    return Expect(
            assembly.received_fragment_count == 1,
            "expected assembly state to remain incomplete after failed double-loss recovery");
}

}  // namespace

int main() {
    if (!TestRecoversSingleMissingFragment()) {
        return EXIT_FAILURE;
    }
    if (!TestRejectsRecoveryWhenTwoFragmentsAreMissing()) {
        return EXIT_FAILURE;
    }
    return EXIT_SUCCESS;
}
