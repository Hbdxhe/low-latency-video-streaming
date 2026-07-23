#ifndef SSERVER_MODULES_TRANSPORT_RTP_RTPSTREAMINGBACKEND_H
#define SSERVER_MODULES_TRANSPORT_RTP_RTPSTREAMINGBACKEND_H

#include <atomic>
#include <chrono>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include <netinet/in.h>

#include "common/metrics/LatencyRecorder.h"
#include "config/AppConfig.h"
#include "modules/transport/ITransportBackend.h"
#include "modules/transport/rtp/RtpPacketizer.h"

// RtpPacketizer:
//   负责把 H.264 EncodedFrame 打成 RTP packets。

// RtpStreamingBackend:
//   负责打开 UDP socket，把这些 RTP packets 真正 sendto 发出去。
namespace sserver {
namespace modules {
namespace transport {
namespace rtp {

class RtpStreamingBackend : public ITransportBackend {
public:
    explicit RtpStreamingBackend(const std::shared_ptr<common::metrics::LatencyRecorder> &send_latency_recorder);
    ~RtpStreamingBackend() override;

    bool initialize(const core::ApplicationContext &context) override;
    bool start() override;
    void stop() override;
    void shutdown() override;
    core::ModuleState state() const override;
    void Broadcast(common::model::EncodedFramePtr frame) override;
    int bound_port() const override;
    TransportBackend backend() const override;
    const std::string &backend_name() const override;

private:
    bool OpenSocket();
    void CloseSocket();

    // 根据配置设置远端 RTP 接收地址。
    bool ConfigureRemoteAddress();

    // 写 SDP 文件。
    //
    // SDP 用来告诉播放器/客户端：
    // 这路 RTP 流是什么编码、端口是多少、payload type 是多少。
    bool WriteSdpFile() const;

    // 计算 RTP packet pacing 间隔。
    //
    // 一帧如果拆成很多 RTP 包，不希望瞬间全发出去，
    // 否则可能造成网络突发流量。
    std::chrono::nanoseconds ComputePacingInterval(
            std::size_t packet_count,
            std::uint64_t current_capture_timestamp_ns);

    // 根据 pacing 间隔等待，让一帧内多个 RTP 包均匀发出
    void PacePacketBurst(
            std::size_t packet_index,
            std::chrono::steady_clock::time_point frame_send_start,
            std::chrono::nanoseconds pacing_interval) const;

private:
    config::TransportConfig config_;
    std::string backend_name_;
    int latency_log_interval_frames_;
    int socket_fd_;
    int bound_port_;

    // RTP 远端接收地址。
    //
    // RTP 后端和 UDP keepalive 后端不一样：
    // UDP 后端是客户端 keepalive 后注册地址；
    // RTP 后端这里是通过配置 rtp_remote_host / rtp_remote_port 指定目标。
    sockaddr_in remote_address_;
    bool has_remote_address_;
    std::shared_ptr<common::metrics::LatencyRecorder> send_latency_recorder_;
    std::unique_ptr<RtpPacketizer> packetizer_;
    std::atomic<core::ModuleState> state_;
    std::uint64_t sent_frames_;
    std::uint64_t sent_packets_;
    std::uint64_t failed_frames_;

    // fallback 帧间隔
    std::uint64_t fallback_frame_interval_ns_;

    // 上一帧采集时间
    std::uint64_t previous_frame_capture_timestamp_ns_;
};

}  // namespace rtp
}  // namespace transport
}  // namespace modules
}  // namespace sserver

#endif  // SSERVER_MODULES_TRANSPORT_RTP_RTPSTREAMINGBACKEND_H
