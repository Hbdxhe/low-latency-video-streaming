#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <string>
#include <unistd.h>

#include "config/AppConfig.h"

namespace {

bool Expect(bool condition, const std::string &message) {
    if (!condition) {
        std::cerr << message << std::endl;
        return false;
    }
    return true;
}

bool WriteTempConfig(const std::string &contents, std::string *path) {
    if (path == nullptr) {
        return false;
    }

    char temp_path[] = "/tmp/sserver-config-negative-XXXXXX.conf";
    const int fd = mkstemps(temp_path, 5);
    if (fd < 0) {
        return false;
    }
    close(fd);

    std::ofstream output(temp_path, std::ios::out | std::ios::trunc);
    if (!output.is_open()) {
        std::remove(temp_path);
        return false;
    }

    output << contents;
    output.close();
    *path = temp_path;
    return true;
}

bool ExpectLoadFailsWithMessage(const std::string &contents, const std::string &message_snippet) {
    std::string config_path;
    if (!WriteTempConfig(contents, &config_path)) {
        std::cerr << "failed to create temporary config file" << std::endl;
        return false;
    }

    sserver::config::AppConfig config;
    std::string error_message;
    const bool loaded = sserver::config::ConfigLoader::LoadFromFile(config_path, &config, &error_message);
    std::remove(config_path.c_str());

    if (!Expect(!loaded, "expected config load to fail")) {
        return false;
    }
    return Expect(
            error_message.find(message_snippet) != std::string::npos,
            "expected error message to contain: " + message_snippet + ", actual: " + error_message);
}

bool TestRejectsConfigLineWithoutEquals() {
    return ExpectLoadFailsWithMessage(
            "app.name = config-loader-negative-test\ntransport.listen_port 1234\n",
            "missing '='");
}

bool TestRejectsUnsupportedConfigKey() {
    return ExpectLoadFailsWithMessage(
            "app.name = config-loader-negative-test\ntransport.unknown_option = true\n",
            "unsupported config key");
}

bool TestRejectsInvalidNullPayloadMode() {
    return ExpectLoadFailsWithMessage(
            "app.name = config-loader-negative-test\ncapture.source = null\ncapture.null_payload_mode = invalid_mode\n",
            "capture.null_payload_mode");
}

bool TestRejectsInvalidQueueDropPolicy() {
    return ExpectLoadFailsWithMessage(
            "app.name = config-loader-negative-test\ntransport.queue_drop_policy = drop_newest\n",
            "transport.queue_drop_policy");
}

bool TestRejectsInvalidX264IntegerSettings() {
    return ExpectLoadFailsWithMessage(
            "app.name = config-loader-negative-test\ncodec.x264_threads = 0\n",
            "codec x264 integer settings must be positive");
}

}  // namespace

int main() {
    if (!TestRejectsConfigLineWithoutEquals()) {
        return EXIT_FAILURE;
    }
    if (!TestRejectsUnsupportedConfigKey()) {
        return EXIT_FAILURE;
    }
    if (!TestRejectsInvalidNullPayloadMode()) {
        return EXIT_FAILURE;
    }
    if (!TestRejectsInvalidQueueDropPolicy()) {
        return EXIT_FAILURE;
    }
    if (!TestRejectsInvalidX264IntegerSettings()) {
        return EXIT_FAILURE;
    }
    return EXIT_SUCCESS;
}
