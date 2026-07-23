#include "modules/transport/rtp/RtpStreamingBackend.h"

#include <arpa/inet.h>
#include <errno.h>
#include <sys/socket.h>
#include <unistd.h>

#include <chrono>
#include <cstring>
#include <fstream>
#include <sstream>
#include <thread>
#include <time.h>

#include "common/log/Logger.h"
#include "common/net/RtpProtocol.h"
#include "common/time/MonotonicClock.h"

namespace sserver {
namespace modules {
namespace transport {
namespace rtp {

namespace {

// 一帧 RTP 包最多分散在 2ms 内发送，避免一帧拆出很多 RTP 包后瞬间打爆网络。
const std::uint64_t kMaxRtpPacingWindowNs = 2ULL * 1000ULL * 1000ULL;

// pacing 窗口最多取帧间隔的 1/4。
// 例如 30fps 一帧约 33ms，1/4 是 8ms，但后面还有最大 2ms 限制。
const std::uint64_t kRtpPacingFrameFractionDivisor = 4ULL;

// 如果计算出的包间隔小于 50us，就不做 pacing。
// 太短的 sleep 精度不稳定，意义也不大。
const std::uint64_t kMinRtpPacingIntervalNs = 50ULL * 1000ULL;

}  // namespace

RtpStreamingBackend::RtpStreamingBackend(
        const std::shared_ptr<common::metrics::LatencyRecorder> &send_latency_recorder)
        : backend_name_("rtp"),
          latency_log_interval_frames_(120),
          socket_fd_(-1),
          bound_port_(0),
          has_remote_address_(false),
          send_latency_recorder_(send_latency_recorder),
          state_(core::ModuleState::kCreated),
          sent_frames_(0),
          sent_packets_(0),
          failed_frames_(0),
          fallback_frame_interval_ns_(0),
          previous_frame_capture_timestamp_ns_(0) {
}

RtpStreamingBackend::~RtpStreamingBackend() {
    shutdown();
}

bool RtpStreamingBackend::initialize(const core::ApplicationContext &context) {
    config_ = context.config.transport;
    latency_log_interval_frames_ = context.config.runtime.latency_log_interval_frames;

    // 计算 fallback 帧间隔。
    //
    // pacing 需要知道“一帧大概间隔多久”。优先用 capture.frame_interval_ms；
    // 如果没有，就用 fps 推算；
    // 如果都没有，就无法 fallback。
    if (context.config.capture.frame_interval_ms > 0) {
        fallback_frame_interval_ns_ =
                static_cast<std::uint64_t>(context.config.capture.frame_interval_ms) * 1000000ULL;
    } else if (context.config.capture.fps > 0) {
        fallback_frame_interval_ns_ = 1000000000ULL / static_cast<std::uint64_t>(context.config.capture.fps);
    } else {
        fallback_frame_interval_ns_ = 0;
    }
    previous_frame_capture_timestamp_ns_ = 0;
    state_ = core::ModuleState::kInitialized;
    return true;
}

bool RtpStreamingBackend::start() {
    // 如果传输没启用，直接标记 running
    if (!config_.enabled) {
        state_ = core::ModuleState::kRunning;
        return true;
    }

    // RTP 后端先配置远端地址，再打开本地 UDP socket
    //
    // 远端地址来自配置，不像 UDP 自定义后端那样等客户端 keepalive 注册。
    if (!ConfigureRemoteAddress() || !OpenSocket()) {
        state_ = core::ModuleState::kFailed;
        return false;
    }

    // 创建 RTP packetizer。
    //
    // 之后 Broadcast(frame) 会调用：
    // packetizer_->Packetize(frame, &packets, &error)
    packetizer_.reset(new RtpPacketizer(
            static_cast<std::uint8_t>(config_.rtp_payload_type),
            static_cast<std::uint32_t>(config_.rtp_clock_rate),
            config_.rtp_max_payload_size,
            static_cast<std::uint32_t>(config_.rtp_ssrc),
            config_.rtp_enable_latency_extension));

    // 可选写 SDP 文件。
    //
    // SDP 给播放器/客户端使用，告诉它：
    // 端口、payload type、H264/90000 等信息。
    if (!WriteSdpFile()) {
        CloseSocket();
        packetizer_.reset();
        state_ = core::ModuleState::kFailed;
        return false;
    }

    state_ = core::ModuleState::kRunning;
    return true;
}

void RtpStreamingBackend::stop() {
    // 释放 RTP packetizer
    packetizer_.reset();
    CloseSocket();

    // 清除远端地址有效标记
    has_remote_address_ = false;

    // 清空上一帧时间
    previous_frame_capture_timestamp_ns_ = 0;
    state_ = core::ModuleState::kStopped;
}

void RtpStreamingBackend::shutdown() {
    stop();
    state_ = core::ModuleState::kShutdown;
}

core::ModuleState RtpStreamingBackend::state() const {
    return state_.load();
}

void RtpStreamingBackend::Broadcast(common::model::EncodedFramePtr frame) {

    // 前置检查。
    //
    // config_.enabled：传输是否启用。
    // frame：是否有有效编码帧。
    // socket_fd_：UDP socket 是否打开。
    // packetizer_：RTP 打包器是否创建。
    // has_remote_address_：远端地址是否配置成功。
    if (!config_.enabled || !frame || socket_fd_ < 0 || !packetizer_ || !has_remote_address_) {
        return;
    }

    // packets 用来保存当前帧打包出来的多个 RTP 包
    std::vector<std::vector<std::uint8_t> > packets;
    std::string error_message;

    // 把 EncodedFrame 打包成 RTP packets。
    //
    // 这里会做：
    // Annex-B H.264 -> NALU
    // 小 NALU -> 单 RTP 包
    // 大 NALU -> FU-A 多 RTP 包
    if (!packetizer_->Packetize(frame, &packets, &error_message)) {
        ++failed_frames_;
        if (!error_message.empty()) {
            common::log::Logger::Warn("rtp packetization failed: " + error_message);
        }
        return;
    }

    // 计算 pacing 间隔。
    //
    // 如果这一帧被拆成很多 RTP 包，可以稍微分散发送，而不是一瞬间全部 sendto。
    const std::chrono::nanoseconds pacing_interval =
            ComputePacingInterval(packets.size(), frame->capture_timestamp_ns);

    // 逐个发送当前帧的 RTP packet。
    const std::chrono::steady_clock::time_point frame_send_start = std::chrono::steady_clock::now();
    for (std::size_t index = 0; index < packets.size(); ++index) {
        std::vector<std::uint8_t> &packet = packets[index];

        // 防止 RTP packet 大于底层 UDP datagram 限制。
        //
        // RTP 仍然跑在 UDP 上。
        // 如果包太大，会增加 IP 分片风险。
        if (packet.size() > config_.udp_max_datagram_size) {
            ++failed_frames_;
            common::log::Logger::Warn("failed to send RTP packet: packet size exceeds transport.udp_max_datagram_size");
            return;
        }

        // 根据 pacing 节奏等待。
        // index=0 不等，后续包按间隔发。
        PacePacketBurst(index, frame_send_start, pacing_interval);

        // 真正发送前记录当前单调时间。
        const std::uint64_t send_timestamp_ns = common::time::MonotonicNowNs();

        // 如果开启 RTP 延迟扩展，则在发送前更新扩展里的发送时间。
        //
        // 为什么不在 Packetize 时写？
        // 因为 Packetize 只是生成包，还不是实际 sendto 的瞬间。
        if (config_.rtp_enable_latency_extension) {
            if (!common::net::UpdateRtpLatencyExtensionTransportSendTimestamp(
                        packet.data(),
                        packet.size(),
                        send_timestamp_ns)) {
                ++failed_frames_;
                common::log::Logger::Warn("failed to update RTP latency header extension before send");
                return;
            }
        }

        // 真正通过 UDP 发送 RTP packet
        const ssize_t sent = sendto(
                socket_fd_,
                packet.data(),
                packet.size(),
                0,
                reinterpret_cast<const sockaddr *>(&remote_address_),
                sizeof(remote_address_));

        // sendto 返回值应该等于 packet.size()。
        // 如果不等，认为发送失败。
        if (sent != static_cast<ssize_t>(packet.size())) {
            ++failed_frames_;
            common::log::Logger::Warn("failed to send RTP packet: " + std::string(std::strerror(errno)));
            return;
        }
        ++sent_packets_;
    }

    ++sent_frames_;

    // 记录 capture_to_send 延迟。
    //
    // 含义：
    // 摄像头采集到这一帧 -> 服务端 RTP 后端发送完成这一帧。
    if (send_latency_recorder_ != nullptr && frame->capture_timestamp_ns != 0) {
        send_latency_recorder_->RecordNs(common::time::MonotonicNowNs() - frame->capture_timestamp_ns);

        // 每隔一定帧数打印统计日志
        if (latency_log_interval_frames_ > 0 &&
            sent_frames_ % static_cast<std::uint64_t>(latency_log_interval_frames_) == 0) {
            std::ostringstream stream;
            stream << "rtp transport stats sent_frames=" << sent_frames_
                   << " sent_packets=" << sent_packets_
                   << " failed_frames=" << failed_frames_;
            common::log::Logger::Info(stream.str());
            common::log::Logger::Info("transport latency rtp-broadcast " +
                                      send_latency_recorder_->Format("capture_to_send"));
        }
    }
}

int RtpStreamingBackend::bound_port() const {
    return bound_port_;
}

TransportBackend RtpStreamingBackend::backend() const {
    return TransportBackend::kRtp;
}

const std::string &RtpStreamingBackend::backend_name() const {
    return backend_name_;
}

bool RtpStreamingBackend::OpenSocket() {
    // 创建 UDP socket。
    //
    // AF_INET：IPv4。
    // SOCK_DGRAM：UDP。
    socket_fd_ = socket(AF_INET, SOCK_DGRAM, 0);
    if (socket_fd_ < 0) {
        common::log::Logger::Error("failed to create RTP UDP socket");
        return false;
    }

    // 设置 UDP 发送缓冲区。
    //
    // RTP 视频流可能会连续发送很多 UDP 包，
    // 增大发送缓冲可以降低发送抖动/失败概率。
    setsockopt(socket_fd_, SOL_SOCKET, SO_SNDBUF, &config_.udp_send_buffer_bytes, sizeof(config_.udp_send_buffer_bytes));

    // 本地绑定地址
    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_port = htons(static_cast<std::uint16_t>(config_.listen_port));
    if (inet_pton(AF_INET, config_.bind_address.c_str(), &address.sin_addr) != 1) {
        common::log::Logger::Error("invalid RTP bind address: " + config_.bind_address);
        CloseSocket();
        return false;
    }

    // bind 本地 IP:端口。
    //
    // 即使主要是 sendto 发送，bind 也有意义：可以固定本地发送端口，也方便 SDP/调试。
    if (bind(socket_fd_, reinterpret_cast<sockaddr *>(&address), sizeof(address)) < 0) {
        common::log::Logger::Error("failed to bind RTP UDP socket");
        CloseSocket();
        return false;
    }

    // 查询实际绑定端口。
    //
    // 如果 listen_port=0，系统会自动分配端口，这里能拿到真实端口。
    sockaddr_in bound_address{};
    socklen_t bound_length = sizeof(bound_address);
    if (getsockname(socket_fd_, reinterpret_cast<sockaddr *>(&bound_address), &bound_length) == 0) {
        bound_port_ = ntohs(bound_address.sin_port);
    } else {
        bound_port_ = config_.listen_port;
    }

    return true;
}

void RtpStreamingBackend::CloseSocket() {
    if (socket_fd_ >= 0) {
        close(socket_fd_);
        socket_fd_ = -1;
    }
    bound_port_ = 0;
}

bool RtpStreamingBackend::ConfigureRemoteAddress() {
    // 清空远端地址结构
    remote_address_ = sockaddr_in();

    // IPv4
    remote_address_.sin_family = AF_INET;

    // 远端 RTP 接收端口。
    // 这个端口通常是客户端或播放器监听 RTP 的端口
    remote_address_.sin_port = htons(static_cast<std::uint16_t>(config_.rtp_remote_port));

    // 远端 IP 字符串转二进制
    if (inet_pton(AF_INET, config_.rtp_remote_host.c_str(), &remote_address_.sin_addr) != 1) {
        common::log::Logger::Error("invalid RTP remote host: " + config_.rtp_remote_host);
        has_remote_address_ = false;
        return false;
    }

    // 标记远端地址可用
    has_remote_address_ = true;
    return true;
}

bool RtpStreamingBackend::WriteSdpFile() const {
    // 如果没配置 SDP 路径，就不写
    if (config_.rtp_sdp_path.empty()) {
        return true;
    }

    // 打开 SDP 文件，覆盖写入
    std::ofstream output(config_.rtp_sdp_path.c_str(), std::ios::out | std::ios::trunc);
    if (!output.is_open()) {
        common::log::Logger::Error("failed to write RTP SDP file: " + config_.rtp_sdp_path);
        return false;
    }

    output << "v=0\n";
    output << "o=- 0 0 IN IP4 " << config_.rtp_remote_host << "\n";
    output << "s=sserver RTP stream\n";
    output << "c=IN IP4 " << config_.rtp_remote_host << "\n";
    output << "t=0 0\n";
    output << "m=video " << config_.rtp_remote_port << " RTP/AVP " << config_.rtp_payload_type << "\n";
    output << "a=rtpmap:" << config_.rtp_payload_type << " H264/" << config_.rtp_clock_rate << "\n";
    output << "a=fmtp:" << config_.rtp_payload_type << " packetization-mode=1\n";
    output << "a=recvonly\n";
    return true;
}

// 根据帧间隔和 packet 数量计算 Pacing 间隔，避免突发流量导致网络拥塞
// 将一帧的多个 RTP packet 分散在一个 pacing 窗口内均匀发送
std::chrono::nanoseconds RtpStreamingBackend::ComputePacingInterval(
        std::size_t packet_count,
        std::uint64_t current_capture_timestamp_ns) {
    // 如果一帧拆出来的包少于 4 个，就不做 pacing。
    //
    // 包很少，直接发出去即可。
    if (packet_count < 4) {
        if (current_capture_timestamp_ns != 0) {
            previous_frame_capture_timestamp_ns_ = current_capture_timestamp_ns;
        }
        return std::chrono::nanoseconds::zero();
    }

    // 默认使用 fallback 帧间隔。
    //
    // 例如 30fps：
    // fallback_frame_interval_ns_ 约等于 33ms。
    std::uint64_t frame_interval_ns = fallback_frame_interval_ns_;

    // 如果当前帧有 capture timestamp，
    // 就优先用当前帧和上一帧的采集时间差计算真实帧间隔。
    if (current_capture_timestamp_ns != 0) {
        if (previous_frame_capture_timestamp_ns_ != 0 &&
            current_capture_timestamp_ns > previous_frame_capture_timestamp_ns_) {
            frame_interval_ns = current_capture_timestamp_ns - previous_frame_capture_timestamp_ns_;
        }

        // 更新上一帧时间
        previous_frame_capture_timestamp_ns_ = current_capture_timestamp_ns;
    }

    // 如果仍然不知道帧间隔，就不做 pacing
    if (frame_interval_ns == 0) {
        return std::chrono::nanoseconds::zero();
    }

    // pacing 窗口取：
    // min(2ms, frame_interval / 4)
    //
    // 意思是：
    // 一帧的 RTP 包最多分散在 2ms 内发送，或者最多占用帧间隔的 1/4。
    const std::uint64_t pacing_window_ns =
            std::min(kMaxRtpPacingWindowNs, frame_interval_ns / kRtpPacingFrameFractionDivisor);

    // 如果窗口太小，不做 pacing
    if (pacing_window_ns < kMinRtpPacingIntervalNs) {
        return std::chrono::nanoseconds::zero();
    }

    // 计算每两个 packet 之间的间隔。
    const std::uint64_t interval_ns = pacing_window_ns / static_cast<std::uint64_t>(packet_count - 1);

    // 单个间隔太小，也不做 pacing
    if (interval_ns < kMinRtpPacingIntervalNs) {
        return std::chrono::nanoseconds::zero();
    }

    return std::chrono::nanoseconds(interval_ns);
}

// 按 pacing 间隔对单个 packet 实施精确等待，确保发送节奏均匀
void RtpStreamingBackend::PacePacketBurst(
        std::size_t packet_index,
        std::chrono::steady_clock::time_point frame_send_start,
        std::chrono::nanoseconds pacing_interval) const {

    // 第一个包立即发送。
    // pacing_interval <= 0 说明不启用 pacing。
    if (packet_index == 0 || pacing_interval <= std::chrono::nanoseconds::zero()) {
        return;
    }

    // 当前 packet 的目标发送时间：
    //
    // frame_send_start + packet_index * pacing_interval
    //
    // 举例：
    // pacing_interval = 200us
    //
    // packet 0: start + 0us
    // packet 1: start + 200us
    // packet 2: start + 400us
    const std::chrono::steady_clock::time_point target_send_time =
            frame_send_start + (pacing_interval * static_cast<std::chrono::nanoseconds::rep>(packet_index));
    const auto now = std::chrono::steady_clock::now();

    // 如果已经过了目标时间，就不等了，直接发
    if (target_send_time <= now) {
        return;
    }

    // 还需要等待多久
    const auto remaining = target_send_time - now;

    // 转成 timespec
    struct timespec ts;
    ts.tv_sec = std::chrono::duration_cast<std::chrono::seconds>(remaining).count();
    ts.tv_nsec = (remaining - std::chrono::duration_cast<std::chrono::seconds>(remaining)).count();

    // 使用 CLOCK_MONOTONIC 等待。
    // 单调时钟不会受系统时间调整影响。
    clock_nanosleep(CLOCK_MONOTONIC, 0, &ts, nullptr);
}

}  // namespace rtp
}  // namespace transport
}  // namespace modules
}  // namespace sserver
