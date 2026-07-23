#include "modules/transport/TransportModule.h"

#include <utility>

#include "common/log/Logger.h"

namespace sserver {
namespace modules {
namespace transport {

// TransportModule 构造函数。
//
// TransportModule 是模块层封装，继承 core::IModule。
// 它内部持有一个 Transport 对象，真正的 TCP / UDP / RTP 后端选择、启动和发送逻辑都由 Transport 负责。
//
// send_latency_recorder 用于记录发送链路延迟，会传递给 Transport，再继续传递给具体传输后端。
TransportModule::TransportModule(const std::shared_ptr<common::metrics::LatencyRecorder> &send_latency_recorder)
        : transport_(std::make_unique<Transport>(send_latency_recorder)) {
}

TransportModule::~TransportModule() {
    shutdown();
}

std::string TransportModule::name() const {
    return "TransportModule";
}

// 初始化传输模块
bool TransportModule::initialize(const core::ApplicationContext &context) {
    std::string error_message;

    // 初始化底层 Transport
    if (!transport_->initialize(context, &error_message)) {
        if (!error_message.empty()) {
            common::log::Logger::Error("failed to initialize transport: " + error_message);
        }
        return false;
    }
    return true;
}

// 启动传输模块
bool TransportModule::start() {
    std::string error_message;

    // 启动底层 Transport
    if (!transport_->start(&error_message)) {
        // 启动失败时打印错误日志
        if (!error_message.empty()) {
            common::log::Logger::Error("failed to start transport: " + error_message);
        }
        return false;
    }

    return true;
}

// 停止传输模块
void TransportModule::stop() {
    transport_->stop();
}

// 关闭传输模块并释放资源
void TransportModule::shutdown() {
    transport_->shutdown();
}


core::ModuleState TransportModule::state() const {
    return transport_->state();
}

void TransportModule::Broadcast(common::model::EncodedFramePtr frame) {
    transport_->Broadcast(frame);
}

int TransportModule::bound_port() const {
    return transport_->bound_port();
}

std::string TransportModule::backend_name() const {
    return transport_->backend_name();
}

}  // namespace transport
}  // namespace modules
}  // namespace sserver
