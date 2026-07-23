#ifndef SSERVER_MODULES_TRANSPORT_TRANSPORTMODULE_H
#define SSERVER_MODULES_TRANSPORT_TRANSPORTMODULE_H

#include <memory>
#include <string>

#include "common/model/EncodedFrame.h"
#include "core/IModule.h"
#include "modules/transport/Transport.h"

namespace sserver {
namespace modules {
namespace transport {

// 传输模块封装类。
//
// TransportModule 继承 core::IModule，表示它是系统中的一个标准模块。
// 它负责把底层 Transport 对象接入统一模块生命周期：
//      initialize()
//      start()
//      stop()
//      shutdown()
//      state()
//
// TransportModule 本身不直接处理 TCP / UDP / RTP 发送逻辑，
// 真正的传输工作由内部的 Transport 对象完成。
class TransportModule : public core::IModule {
public:
    explicit TransportModule(const std::shared_ptr<common::metrics::LatencyRecorder> &send_latency_recorder);
    ~TransportModule() override;

    std::string name() const override;
    bool initialize(const core::ApplicationContext &context) override;
    bool start() override;
    void stop() override;
    void shutdown() override;
    core::ModuleState state() const override;

    void Broadcast(common::model::EncodedFramePtr frame);
    int bound_port() const;
    std::string backend_name() const;

private:
    std::unique_ptr<Transport> transport_;
};

}  // namespace transport
}  // namespace modules
}  // namespace sserver

#endif  // SSERVER_MODULES_TRANSPORT_TRANSPORTMODULE_H
