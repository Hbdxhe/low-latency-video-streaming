#include <iostream>
#include <string>
#include <vector>

#include "core/Application.h"
#include "core/ApplicationContext.h"
#include "core/IModule.h"
#include "core/ModuleState.h"

namespace {

bool Expect(bool condition, const std::string &message) {
    if (!condition) {
        std::cerr << message << std::endl;
        return false;
    }
    return true;
}

struct ModuleCallRecord {
    std::string module_name;
    std::string method;
};

class RecordingModule : public sserver::core::IModule {
public:
    explicit RecordingModule(const std::string &name, std::vector<ModuleCallRecord> *log)
            : name_(name), log_(log), state_(sserver::core::ModuleState::kCreated) {}

    std::string name() const override { return name_; }

    bool initialize(const sserver::core::ApplicationContext & /*context*/) override {
        log_->push_back({name_, "initialize"});
        state_ = sserver::core::ModuleState::kInitialized;
        return true;
    }

    bool start() override {
        log_->push_back({name_, "start"});
        state_ = sserver::core::ModuleState::kRunning;
        return true;
    }

    void stop() override {
        log_->push_back({name_, "stop"});
        state_ = sserver::core::ModuleState::kStopped;
    }

    void shutdown() override {
        log_->push_back({name_, "shutdown"});
        state_ = sserver::core::ModuleState::kShutdown;
    }

    sserver::core::ModuleState state() const override { return state_; }

private:
    std::string name_;
    std::vector<ModuleCallRecord> *log_;
    sserver::core::ModuleState state_;
};

class FailStartModule : public sserver::core::IModule {
public:
    explicit FailStartModule(const std::string &name, std::vector<ModuleCallRecord> *log)
            : name_(name), log_(log), state_(sserver::core::ModuleState::kCreated) {}

    std::string name() const override { return name_; }

    bool initialize(const sserver::core::ApplicationContext & /*context*/) override {
        log_->push_back({name_, "initialize"});
        state_ = sserver::core::ModuleState::kInitialized;
        return true;
    }

    bool start() override {
        log_->push_back({name_, "start"});
        state_ = sserver::core::ModuleState::kFailed;
        return false;
    }

    void stop() override {
        log_->push_back({name_, "stop"});
    }

    void shutdown() override {
        log_->push_back({name_, "shutdown"});
    }

    sserver::core::ModuleState state() const override { return state_; }

private:
    std::string name_;
    std::vector<ModuleCallRecord> *log_;
    sserver::core::ModuleState state_;
};

class FailInitializeModule : public sserver::core::IModule {
public:
    explicit FailInitializeModule(const std::string &name, std::vector<ModuleCallRecord> *log)
            : name_(name), log_(log), state_(sserver::core::ModuleState::kCreated) {}

    std::string name() const override { return name_; }

    bool initialize(const sserver::core::ApplicationContext & /*context*/) override {
        log_->push_back({name_, "initialize"});
        state_ = sserver::core::ModuleState::kFailed;
        return false;
    }

    bool start() override {
        log_->push_back({name_, "start"});
        return true;
    }

    void stop() override {
        log_->push_back({name_, "stop"});
    }

    void shutdown() override {
        log_->push_back({name_, "shutdown"});
    }

    sserver::core::ModuleState state() const override { return state_; }

private:
    std::string name_;
    std::vector<ModuleCallRecord> *log_;
    sserver::core::ModuleState state_;
};

bool TestStartFailureRollsBackPreviouslyStartedModules() {
    std::vector<ModuleCallRecord> log;
    sserver::config::AppConfig config;
    sserver::core::Application app(config);

    app.RegisterModule(std::make_shared<RecordingModule>("moduleA", &log));
    app.RegisterModule(std::make_shared<RecordingModule>("moduleB", &log));
    app.RegisterModule(std::make_shared<FailStartModule>("moduleC", &log));

    const bool started = app.Start();
    if (!Expect(!started, "expected Start() to return false when moduleC fails to start")) {
        return false;
    }

    bool found_moduleB_stop = false;
    bool found_moduleA_stop = false;
    for (const auto &entry : log) {
        if (entry.module_name == "moduleB" && entry.method == "stop") {
            found_moduleB_stop = true;
        }
        if (entry.module_name == "moduleA" && entry.method == "stop") {
            found_moduleA_stop = true;
        }
    }

    if (!Expect(found_moduleB_stop, "expected moduleB.stop() to be called on rollback")) {
        return false;
    }
    if (!Expect(found_moduleA_stop, "expected moduleA.stop() to be called on rollback")) {
        return false;
    }

    // 验证回滚顺序：moduleB 应先于 moduleA 停止
    int moduleB_stop_idx = -1;
    int moduleA_stop_idx = -1;
    for (int i = 0; i < static_cast<int>(log.size()); ++i) {
        if (log[i].module_name == "moduleB" && log[i].method == "stop") {
            moduleB_stop_idx = i;
        }
        if (log[i].module_name == "moduleA" && log[i].method == "stop") {
            moduleA_stop_idx = i;
        }
    }
    return Expect(moduleB_stop_idx < moduleA_stop_idx,
                  "expected moduleB to be stopped before moduleA (reverse order)");
}

bool TestInitializeFailureRollsBackPreviouslyInitializedModules() {
    std::vector<ModuleCallRecord> log;
    sserver::config::AppConfig config;
    sserver::core::Application app(config);

    app.RegisterModule(std::make_shared<RecordingModule>("moduleA", &log));
    app.RegisterModule(std::make_shared<RecordingModule>("moduleB", &log));
    app.RegisterModule(std::make_shared<FailInitializeModule>("moduleC", &log));

    const bool initialized = app.Initialize();
    if (!Expect(!initialized, "expected Initialize() to return false when moduleC fails")) {
        return false;
    }

    bool found_moduleB_shutdown = false;
    bool found_moduleA_shutdown = false;
    for (const auto &entry : log) {
        if (entry.module_name == "moduleB" && entry.method == "shutdown") {
            found_moduleB_shutdown = true;
        }
        if (entry.module_name == "moduleA" && entry.method == "shutdown") {
            found_moduleA_shutdown = true;
        }
    }

    if (!Expect(found_moduleB_shutdown, "expected moduleB.shutdown() to be called on rollback")) {
        return false;
    }
    if (!Expect(found_moduleA_shutdown, "expected moduleA.shutdown() to be called on rollback")) {
        return false;
    }

    return true;
}

bool TestSuccessfulStartDoesNotTriggerRollback() {
    std::vector<ModuleCallRecord> log;
    sserver::config::AppConfig config;
    sserver::core::Application app(config);

    app.RegisterModule(std::make_shared<RecordingModule>("moduleA", &log));
    app.RegisterModule(std::make_shared<RecordingModule>("moduleB", &log));

    const bool started = app.Start();
    if (!Expect(started, "expected Start() to succeed")) {
        return false;
    }

    bool found_stop = false;
    for (const auto &entry : log) {
        if (entry.method == "stop") {
            found_stop = true;
        }
    }
    return Expect(!found_stop, "expected no stop() calls during successful start");
}

bool TestFirstModuleFailsStartNoRollbackNeeded() {
    std::vector<ModuleCallRecord> log;
    sserver::config::AppConfig config;
    sserver::core::Application app(config);

    app.RegisterModule(std::make_shared<FailStartModule>("moduleA", &log));

    const bool started = app.Start();
    if (!Expect(!started, "expected Start() to return false")) {
        return false;
    }

    bool found_stop = false;
    for (const auto &entry : log) {
        if (entry.method == "stop") {
            found_stop = true;
        }
    }
    return Expect(!found_stop, "expected no stop() calls when first module fails");
}

}  // namespace

int main() {
    if (!TestStartFailureRollsBackPreviouslyStartedModules()) {
        return EXIT_FAILURE;
    }
    if (!TestInitializeFailureRollsBackPreviouslyInitializedModules()) {
        return EXIT_FAILURE;
    }
    if (!TestSuccessfulStartDoesNotTriggerRollback()) {
        return EXIT_FAILURE;
    }
    if (!TestFirstModuleFailsStartNoRollbackNeeded()) {
        return EXIT_FAILURE;
    }
    return EXIT_SUCCESS;
}
