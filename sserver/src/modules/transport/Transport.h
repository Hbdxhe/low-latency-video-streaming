#ifndef SSERVER_MODULES_TRANSPORT_TRANSPORT_H
#define SSERVER_MODULES_TRANSPORT_TRANSPORT_H

#include <memory>
#include <string>

#include "common/metrics/LatencyRecorder.h"
#include "common/model/EncodedFrame.h"
#include "config/AppConfig.h"
#include "core/ApplicationContext.h"
#include "core/ModuleState.h"

namespace sserver {
namespace modules {
namespace transport {

// 传输后端接口的前向声明。
//
// 这里只声明 ITransportBackend 类名，避免在头文件中直接包含具体后端实现。
// 具体接口定义通常在 ITransportBackend.h 中。
class ITransportBackend;

// 传输后端类型枚举
enum class TransportBackend {
    kAuto,
    kTcp,
    kUdp,
    kRtp,
};

// 传输后端选择结果
struct TransportBackendSelection {
    TransportBackend backend = TransportBackend::kAuto;
    std::string backend_name = "auto";
};

// 根据配置解析传输后端
inline bool ResolveTransportBackendSelection(
        const config::TransportConfig &config,
        TransportBackendSelection *selection,
        std::string *error_message) {

    // selection 是输出参数，必须有效。
    if (selection == nullptr) {
        if (error_message != nullptr) {
            *error_message = "transport backend selection output is null";
        }
        return false;
    }

    // 解析 TCP 后端
    if (config.backend == "tcp") {
        selection->backend = TransportBackend::kTcp;
        selection->backend_name = "tcp";
        return true;
    }

    // 解析 UDP 后端
    if (config.backend == "udp") {
        selection->backend = TransportBackend::kUdp;
        selection->backend_name = "udp";
        return true;
    }

    // 解析 RTP 后端
    if (config.backend == "rtp") {
        selection->backend = TransportBackend::kRtp;
        selection->backend_name = "rtp";
        return true;
    }

    // 如果配置不是 tcp、udp 或 rtp，则认为配置非法
    if (error_message != nullptr) {
        *error_message = "transport.backend must be one of 'tcp', 'udp' or 'rtp'";
    }
    return false;
}

// 传输后端工厂，该工厂根据 TransportBackendSelection 创建具体传输后端对象
class TransportBackendFactory {
public:
    // 创建具体传输后端对象。
    //
    // 参数说明：
    // 1. selection
    //      已解析出的传输后端类型。
    //
    // 2. send_latency_recorder
    //      发送延迟统计器。
    //      后端在发送帧时，可以用它记录发送耗时、排队耗时或端到端延迟指标。
    //
    // 3. error_message
    //      创建失败时返回错误信息，可以为 nullptr。
    //
    // 返回值：
    //      成功：返回具体传输后端对象；
    //      失败：返回 nullptr。
    static std::unique_ptr<ITransportBackend> Create(
            const TransportBackendSelection &selection,
            const std::shared_ptr<common::metrics::LatencyRecorder> &send_latency_recorder,
            std::string *error_message);
};

class Transport {
public:
    // 构造函数。
    // send_latency_recorder 用于记录发送链路相关延迟。
    // 使用 shared_ptr 是因为该统计器可能被多个模块共享
    explicit Transport(const std::shared_ptr<common::metrics::LatencyRecorder> &send_latency_recorder);
    ~Transport();

    bool initialize(const core::ApplicationContext &context, std::string *error_message);
    bool start(std::string *error_message);
    void stop();
    void shutdown();
    core::ModuleState state() const;

    void Broadcast(common::model::EncodedFramePtr frame);
    int bound_port() const;
    TransportBackend backend() const;
    const std::string &backend_name() const;

private:
    struct TransportImpl;
    std::unique_ptr<TransportImpl> impl_;
};

}  // namespace transport
}  // namespace modules
}  // namespace sserver

#endif  // SSERVER_MODULES_TRANSPORT_TRANSPORT_H
