#include "modules/transport/Transport.h"

#include <utility>

#include "modules/transport/ITransportBackend.h"
#include "modules/transport/rtp/RtpStreamingBackend.h"
#include "modules/transport/tcp/TcpStreamingBackend.h"
#include "modules/transport/udp/UdpStreamingBackend.h"

namespace sserver {
namespace modules {
namespace transport {

// 根据后端选择结果创建具体传输后端对象
std::unique_ptr<ITransportBackend> TransportBackendFactory::Create(
        const TransportBackendSelection &selection,
        const std::shared_ptr<common::metrics::LatencyRecorder> &send_latency_recorder,
        std::string *error_message) {

    switch (selection.backend) {
        case TransportBackend::kTcp:
            // 创建 TCP 传输后端。
            //
            // TCP 可靠、有序，但实时性可能受重传影响。
            // send_latency_recorder 用于记录发送链路延迟。
            return std::unique_ptr<ITransportBackend>(new tcp::TcpStreamingBackend(send_latency_recorder));

        case TransportBackend::kUdp:
            // 创建 UDP 传输后端。
            //
            // UDP 不保证可靠性和有序性，但延迟低，适合实时传输场景。
            return std::unique_ptr<ITransportBackend>(new udp::UdpStreamingBackend(send_latency_recorder));

        case TransportBackend::kRtp:
            // 创建 RTP 传输后端。
            //
            // RTP 通常基于 UDP，用于音视频实时传输。
            // H.264 视频帧一般需要在 RTP 后端中进行分包发送。
            return std::unique_ptr<ITransportBackend>(new rtp::RtpStreamingBackend(send_latency_recorder));

        case TransportBackend::kAuto:
        default:
            if (error_message != nullptr) {
                *error_message = "requested transport backend is not supported";
            }
            return nullptr;
    }
}

// Transport 的私有实现结构体
struct Transport::TransportImpl {
    // 用户配置中请求的传输后端
    TransportBackend requested_backend = TransportBackend::kAuto;
    std::string requested_backend_name = "auto";

    // 实际启用的传输后端
    TransportBackend active_backend = TransportBackend::kTcp;
    std::string active_backend_name = "tcp";

    // 具体传输后端对象
    std::unique_ptr<ITransportBackend> backend;

    // 发送延迟统计器
    std::shared_ptr<common::metrics::LatencyRecorder> send_latency_recorder;

    // Transport 当前模块状态
    core::ModuleState state = core::ModuleState::kCreated;
};

// Transport 构造函数。创建内部 TransportImpl 对象，并保存发送延迟统计器。
Transport::Transport(const std::shared_ptr<common::metrics::LatencyRecorder> &send_latency_recorder)
        : impl_(std::make_unique<TransportImpl>()) {
    impl_->send_latency_recorder = send_latency_recorder;
}

Transport::~Transport() {
    shutdown();
}

// 初始化传输模块
bool Transport::initialize(const core::ApplicationContext &context, std::string *error_message) {
    // 允许重复初始化，每次初始化前先释放旧 backend，避免资源残留。
    shutdown();

    // 根据配置文件中的 context.config.transport.backend 解析需要使用的传输后端。
    TransportBackendSelection selection;
    if (!ResolveTransportBackendSelection(context.config.transport, &selection, error_message)) {
        impl_->state = core::ModuleState::kFailed;
        return false;
    }

    // 记录用户请求的传输后端
    impl_->requested_backend = selection.backend;
    impl_->requested_backend_name = selection.backend_name;

    // 根据 selection 创建具体后端对象
    impl_->backend = TransportBackendFactory::Create(selection, impl_->send_latency_recorder, error_message);
    if (!impl_->backend) {
        impl_->state = core::ModuleState::kFailed;
        return false;
    }

    // 初始化具体传输后端
    if (!impl_->backend->initialize(context)) {
        if (error_message != nullptr && error_message->empty()) {
            *error_message = "transport backend failed to initialize";
        }

        // 初始化失败时释放后端对象，避免保留半初始化状态
        impl_->backend.reset();
        impl_->state = core::ModuleState::kFailed;
        return false;
    }

    // 初始化成功后，记录实际启用的后端类型和名称
    impl_->active_backend = impl_->backend->backend();
    impl_->active_backend_name = impl_->backend->backend_name();

    // 同步后端状态
    impl_->state = impl_->backend->state();
    return true;
}

// 启动传输模块
bool Transport::start(std::string *error_message) {
    // 如果 backend 为空，说明还没有成功 initialize()
    if (!impl_->backend) {
        impl_->state = core::ModuleState::kFailed;
        if (error_message != nullptr) {
            *error_message = "transport is not initialized";
        }
        return false;
    }

    // 启动具体传输后端
    if (!impl_->backend->start()) {
        if (error_message != nullptr && error_message->empty()) {
            *error_message = "transport backend failed to start";
        }
        // 启动失败时同步后端状态
        impl_->state = impl_->backend->state();
        return false;
    }

    // 启动成功后同步后端状态
    impl_->state = impl_->backend->state();
    return true;
}

// 停止传输模块
void Transport::stop() {
    if (impl_->backend) {
        impl_->backend->stop();

        // 同步后端状态
        impl_->state = impl_->backend->state();
        return;
    }

    // 如果 backend 不存在，则直接把 Transport 状态置为 stopped
    impl_->state = core::ModuleState::kStopped;
}

// 关闭传输模块并释放资源
void Transport::shutdown() {
    if (impl_->backend) {
        // 释放具体后端内部资源
        impl_->backend->shutdown();

        // 销毁后端对象
        impl_->backend.reset();
    }

    impl_->requested_backend = TransportBackend::kAuto;
    impl_->requested_backend_name = "auto";
    impl_->active_backend = TransportBackend::kTcp;
    impl_->active_backend_name = "tcp";
    impl_->state = core::ModuleState::kShutdown;
}

// 查询当前传输模块状态
core::ModuleState Transport::state() const {
    // 如果后端存在，优先返回后端真实状态
    if (impl_->backend) {
        return impl_->backend->state();
    }

    // 如果没有后端，则返回 Transport 自己记录的状态
    return impl_->state;
}

// 广播一帧编码后的数据
void Transport::Broadcast(common::model::EncodedFramePtr frame) {
    if (impl_->backend) {
        impl_->backend->Broadcast(frame);
    }
}

// 返回当前实际绑定端口
int Transport::bound_port() const {
    if (!impl_->backend) {
        return 0;
    }
    return impl_->backend->bound_port();
}

// 返回当前记录的实际传输后端类型
TransportBackend Transport::backend() const {
    return impl_->active_backend;
}

// 返回当前记录的实际传输后端名称
const std::string &Transport::backend_name() const {
    return impl_->active_backend_name;
}

}  // namespace transport
}  // namespace modules
}  // namespace sserver
