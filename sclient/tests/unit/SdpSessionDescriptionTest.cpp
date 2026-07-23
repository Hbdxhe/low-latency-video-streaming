#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <string>
#include <unistd.h>

#include "common/net/SdpSessionDescription.h"
#include "tests/support/TestAssertions.h"

namespace {

using sclient::tests::support::Expect;

bool TestParsesSserverRtpSdp() {
    const std::string sdp =
            "v=0\n"
            "o=- 0 0 IN IP4 127.0.0.1\n"
            "s=sserver RTP stream\n"
            "c=IN IP4 127.0.0.1\n"
            "t=0 0\n"
            "m=video 19504 RTP/AVP 96\n"
            "a=rtpmap:96 H264/90000\n"
            "a=fmtp:96 packetization-mode=1\n"
            "a=recvonly\n";

    sclient::common::net::RtpVideoSessionDescription description;
    std::string error_message;
    if (!sclient::common::net::ParseRtpVideoSessionDescription(sdp, &description, &error_message)) {
        return Expect(false, "expected SDP to parse: " + error_message);
    }

    return Expect(description.connection_address == "127.0.0.1", "expected SDP connection address") &&
           Expect(description.video_port == 19504, "expected SDP video port") &&
           Expect(description.payload_type == 96, "expected SDP payload type") &&
           Expect(description.clock_rate == 90000, "expected SDP clock rate");
}

bool TestLoadsSdpFromFile() {
    char path_template[] = "/tmp/sclient_sdp_test_XXXXXX";
    const int file_descriptor = mkstemp(path_template);
    if (file_descriptor < 0) {
        return Expect(false, "failed to create temporary SDP file");
    }
    close(file_descriptor);

    const std::string file_path(path_template);
    {
        std::ofstream output(file_path.c_str(), std::ios::out | std::ios::trunc);
        output << "v=0\n"
               << "c=IN IP4 127.0.0.1\n"
               << "m=video 19514 RTP/AVP 97\n"
               << "a=rtpmap:97 H264/90000\n";
    }

    sclient::common::net::RtpVideoSessionDescription description;
    std::string error_message;
    const bool loaded =
            sclient::common::net::LoadRtpVideoSessionDescription(file_path, &description, &error_message);
    std::remove(file_path.c_str());
    if (!loaded) {
        return Expect(false, "expected SDP file to load: " + error_message);
    }

    return Expect(description.connection_address == "127.0.0.1", "expected loaded SDP connection address") &&
           Expect(description.video_port == 19514, "expected loaded SDP video port") &&
           Expect(description.payload_type == 97, "expected loaded SDP payload type") &&
           Expect(description.clock_rate == 90000, "expected loaded SDP clock rate");
}

bool TestRejectsIncompleteSdp() {
    sclient::common::net::RtpVideoSessionDescription description;
    std::string error_message;
    const bool parsed = sclient::common::net::ParseRtpVideoSessionDescription(
            "v=0\nm=video 19504 RTP/AVP 96\n",
            &description,
            &error_message);
    return Expect(!parsed, "expected incomplete SDP to be rejected") &&
           Expect(!error_message.empty(), "expected parse failure to describe the missing SDP field");
}

}  // namespace

int main() {
    if (!TestParsesSserverRtpSdp()) {
        return EXIT_FAILURE;
    }
    if (!TestLoadsSdpFromFile()) {
        return EXIT_FAILURE;
    }
    if (!TestRejectsIncompleteSdp()) {
        return EXIT_FAILURE;
    }
    return EXIT_SUCCESS;
}
