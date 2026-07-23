#include <chrono>
#include <cstring>
#include <string>
#include <thread>

#include "app/AppBootstrap.h"
#include "common/log/Logger.h"
#include "config/AppConfig.h"

namespace {

std::string ParseConfigPath(int argc, char **argv) {
    std::string config_path = std::string(SSERVER_SOURCE_DIR) + "/config/smoke.conf";
    for (int index = 1; index < argc; ++index) {
        if (std::strcmp(argv[index], "--config") == 0 && index + 1 < argc) {
            config_path = argv[index + 1];
            ++index;
        }
    }
    return config_path;
}

}  // namespace

int main(int argc, char **argv) {
    const std::string config_path = ParseConfigPath(argc, argv);

    sserver::config::AppConfig config;
    std::string error_message;
    if (!sserver::config::ConfigLoader::LoadFromFile(config_path, &config, &error_message)) {
        sserver::common::log::Logger::Error("failed to load config: " + error_message);
        return 1;
    }

    const std::string log_file_path = sserver::common::log::Logger::CurrentLogFilePath();
    if (!log_file_path.empty()) {
        sserver::common::log::Logger::Info("log file: " + log_file_path);
    }
    sserver::common::log::Logger::Info("smoke test starting with config: " + config_path);

    sserver::app::AppBootstrap bootstrap(config);
    if (!bootstrap.Initialize()) {
        sserver::common::log::Logger::Error("failed to initialize smoke test application");
        return 2;
    }
    if (!bootstrap.Start()) {
        sserver::common::log::Logger::Error("failed to start smoke test application");
        return 3;
    }

    std::this_thread::sleep_for(std::chrono::milliseconds(120));
    bootstrap.Stop();
    sserver::common::log::Logger::Info("smoke test completed successfully");
    return 0;
}
