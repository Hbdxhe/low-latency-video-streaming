#include <iostream>
#include <string>
#include <vector>

#include "app/cli/CliOptions.h"
#include "tests/support/TestAssertions.h"

namespace {

using sclient::tests::support::Expect;

struct ArgvStorage {
    std::vector<std::string> values;
    std::vector<char *> argv;

    explicit ArgvStorage(std::initializer_list<const char *> args) {
        values.assign(args.begin(), args.end());
        argv.reserve(values.size());
        for (std::size_t index = 0; index < values.size(); ++index) {
            argv.push_back(&values[index][0]);
        }
    }

    int argc() const {
        return static_cast<int>(argv.size());
    }

    char **data() {
        return argv.data();
    }
};

bool TestParseSharedStreamOptions() {
    sclient::ClientConfig config;
    sclient::DecodeBackend decode_backend = sclient::DecodeBackend::kAuto;
    ArgvStorage args({"cli_options_test", "--host", "10.0.0.8", "--sdp", "demo.sdp", "--udp-jitter-buffer", "off", "--decoder", "software"});

    int index = 1;
    sclient::CliParseResult result = sclient::ParseSharedStreamOption(
            args.argc(),
            args.data(),
            &index,
            &config,
            &decode_backend);
    if (!Expect(result.handled && result.success, "expected --host to parse successfully")) {
        return false;
    }
    if (!Expect(config.host == "10.0.0.8", "expected host to be updated")) {
        return false;
    }

    index = 3;
    result = sclient::ParseSharedStreamOption(
            args.argc(),
            args.data(),
            &index,
            &config,
            &decode_backend);
    if (!Expect(result.handled && result.success, "expected --sdp to parse successfully")) {
        return false;
    }
    if (!Expect(config.sdp_path == "demo.sdp", "expected sdp path to be updated")) {
        return false;
    }

    index = 5;
    result = sclient::ParseSharedStreamOption(
            args.argc(),
            args.data(),
            &index,
            &config,
            &decode_backend);
    if (!Expect(result.handled && result.success, "expected --udp-jitter-buffer to parse successfully")) {
        return false;
    }
    if (!Expect(!config.udp_jitter_buffer_enabled, "expected udp jitter buffer to be disabled")) {
        return false;
    }

    index = 7;
    result = sclient::ParseSharedStreamOption(
            args.argc(),
            args.data(),
            &index,
            &config,
            &decode_backend);
    if (!Expect(result.handled && result.success, "expected --decoder to parse successfully")) {
        return false;
    }
    return Expect(decode_backend == sclient::DecodeBackend::kSoftware, "expected software decoder");
}

bool TestClientOptionParsesRendererFlags() {
    sclient::ClientConfig config;
    sclient::DecodeBackend decode_backend = sclient::DecodeBackend::kAuto;
    sclient::RenderBackend render_backend = sclient::RenderBackend::kAuto;
    bool vsync_enabled = false;
    std::string window_title = "sclient";
    std::size_t receive_queue_capacity = 8;
    std::size_t decode_queue_capacity = 3;
    ArgvStorage args({"cli_options_test", "--transport", "rtp", "--renderer", "opengl", "--vsync", "on", "--window-title", "demo"});

    int index = 1;
    sclient::CliParseResult result = sclient::ParseClientOption(
            args.argc(),
            args.data(),
            &index,
            &config,
            &decode_backend,
            &render_backend,
            &vsync_enabled,
            &window_title,
            &receive_queue_capacity,
            &decode_queue_capacity);
    if (!Expect(result.handled && result.success, "expected --transport to parse successfully")) {
        return false;
    }
    if (!Expect(config.transport == "rtp", "expected transport to be rtp")) {
        return false;
    }

    index = 3;
    result = sclient::ParseClientOption(
            args.argc(),
            args.data(),
            &index,
            &config,
            &decode_backend,
            &render_backend,
            &vsync_enabled,
            &window_title,
            &receive_queue_capacity,
            &decode_queue_capacity);
    if (!Expect(result.handled && result.success, "expected --renderer to parse successfully")) {
        return false;
    }
    if (!Expect(render_backend == sclient::RenderBackend::kOpenGl, "expected OpenGL renderer")) {
        return false;
    }

    index = 5;
    result = sclient::ParseClientOption(
            args.argc(),
            args.data(),
            &index,
            &config,
            &decode_backend,
            &render_backend,
            &vsync_enabled,
            &window_title,
            &receive_queue_capacity,
            &decode_queue_capacity);
    if (!Expect(result.handled && result.success, "expected --vsync to parse successfully")) {
        return false;
    }
    if (!Expect(vsync_enabled, "expected vsync to be enabled")) {
        return false;
    }

    index = 7;
    result = sclient::ParseClientOption(
            args.argc(),
            args.data(),
            &index,
            &config,
            &decode_backend,
            &render_backend,
            &vsync_enabled,
            &window_title,
            &receive_queue_capacity,
            &decode_queue_capacity);
    if (!Expect(result.handled && result.success, "expected --window-title to parse successfully")) {
        return false;
    }
    return Expect(window_title == "demo", "expected window title to be updated");
}

bool TestUdpBenchmarkSpecificOptions() {
    sclient::ClientConfig config;
    config.transport = "udp";
    sclient::DecodeBackend decode_backend = sclient::DecodeBackend::kAuto;
    int frames_to_measure = 240;
    bool decode_frames = false;
    ArgvStorage args({"cli_options_test", "--frames", "120", "--decode", "on", "--inject-jitter-pattern", "burst", "--inject-jitter-period", "9"});

    int index = 1;
    sclient::CliParseResult result = sclient::ParseUdpBenchmarkOption(
            args.argc(),
            args.data(),
            &index,
            &config,
            &decode_backend,
            &frames_to_measure,
            &decode_frames);
    if (!Expect(result.handled && result.success, "expected --frames to parse successfully")) {
        return false;
    }
    if (!Expect(frames_to_measure == 120, "expected frame count to be updated")) {
        return false;
    }

    index = 3;
    result = sclient::ParseUdpBenchmarkOption(
            args.argc(),
            args.data(),
            &index,
            &config,
            &decode_backend,
            &frames_to_measure,
            &decode_frames);
    if (!Expect(result.handled && result.success, "expected --decode to parse successfully")) {
        return false;
    }
    if (!Expect(decode_frames, "expected benchmark decoding to be enabled")) {
        return false;
    }

    index = 5;
    result = sclient::ParseUdpBenchmarkOption(
            args.argc(),
            args.data(),
            &index,
            &config,
            &decode_backend,
            &frames_to_measure,
            &decode_frames);
    if (!Expect(result.handled && result.success, "expected --inject-jitter-pattern to parse successfully")) {
        return false;
    }
    if (!Expect(config.udp_test_jitter_pattern == "burst", "expected jitter pattern to be burst")) {
        return false;
    }

    index = 7;
    result = sclient::ParseUdpBenchmarkOption(
            args.argc(),
            args.data(),
            &index,
            &config,
            &decode_backend,
            &frames_to_measure,
            &decode_frames);
    if (!Expect(result.handled && result.success, "expected --inject-jitter-period to parse successfully")) {
        return false;
    }
    return Expect(config.udp_test_jitter_period == 9, "expected jitter period to be updated");
}

bool TestInvalidAndHelpPaths() {
    sclient::ClientConfig config;
    sclient::DecodeBackend decode_backend = sclient::DecodeBackend::kAuto;
    sclient::RenderBackend render_backend = sclient::RenderBackend::kAuto;
    bool vsync_enabled = false;
    std::string window_title = "sclient";
    std::size_t receive_queue_capacity = 8;
    std::size_t decode_queue_capacity = 3;
    ArgvStorage invalid_renderer_args({"cli_options_test", "--renderer", "bogus"});

    int index = 1;
    sclient::CliParseResult result = sclient::ParseClientOption(
            invalid_renderer_args.argc(),
            invalid_renderer_args.data(),
            &index,
            &config,
            &decode_backend,
            &render_backend,
            &vsync_enabled,
            &window_title,
            &receive_queue_capacity,
            &decode_queue_capacity);
    if (!Expect(result.handled && !result.success, "expected invalid renderer to fail")) {
        return false;
    }
    if (!Expect(result.error_message == "invalid value for --renderer", "expected invalid renderer message")) {
        return false;
    }

    ArgvStorage help_args({"cli_options_test", "--help"});
    int frames_to_measure = 240;
    bool decode_frames = false;
    index = 1;
    result = sclient::ParseUdpBenchmarkOption(
            help_args.argc(),
            help_args.data(),
            &index,
            &config,
            &decode_backend,
            &frames_to_measure,
            &decode_frames);
    return Expect(result.handled && result.show_help && result.success, "expected --help to be recognized");
}

}  // namespace

int main() {
    if (!TestParseSharedStreamOptions()) {
        return 1;
    }
    if (!TestClientOptionParsesRendererFlags()) {
        return 1;
    }
    if (!TestUdpBenchmarkSpecificOptions()) {
        return 1;
    }
    if (!TestInvalidAndHelpPaths()) {
        return 1;
    }
    return 0;
}
