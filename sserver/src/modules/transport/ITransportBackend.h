#ifndef SSERVER_MODULES_TRANSPORT_ITRANSPORTBACKEND_H
#define SSERVER_MODULES_TRANSPORT_ITRANSPORTBACKEND_H

#include <memory>
#include <string>

#include "common/model/EncodedFrame.h"
#include "core/ApplicationContext.h"
#include "core/ModuleState.h"
#include "modules/transport/Transport.h"

namespace sserver {
namespace modules {
namespace transport {

// 传输后端抽象接口。
//
// ITransportBackend 不直接实现具体传输逻辑，而是定义一组统一的传输后端接口。
// 上层 TransportModule 只依赖 ITransportBackend，不需要关心底层到底使用 TCP、UDP、RTP、RTMP 还是 WebRTC。

class ITransportBackend {
public:
    virtual ~ITransportBackend() = default;

    virtual bool initialize(const core::ApplicationContext &context) = 0;
    virtual bool start() = 0;
    virtual void stop() = 0;
    virtual void shutdown() = 0;

    // 返回当前模块状态
    virtual core::ModuleState state() const = 0;

    // 广播一帧编码后的音视频数据
    virtual void Broadcast(common::model::EncodedFramePtr frame) = 0;

    // 返回当前后端实际绑定的端口
    virtual int bound_port() const = 0;

    // 返回当前传输后端名称
    virtual TransportBackend backend() const = 0;
    virtual const std::string &backend_name() const = 0;
};

}  // namespace transport
}  // namespace modules
}  // namespace sserver

#endif  // SSERVER_MODULES_TRANSPORT_ITRANSPORTBACKEND_H
