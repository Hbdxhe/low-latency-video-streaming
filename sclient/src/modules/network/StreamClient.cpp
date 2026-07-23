#include "modules/network/StreamClientInternal.h"

#include <chrono>

namespace sclient {

using network_internal::BuildSocketError;
using network_internal::MonotonicNowNs;

StreamClient::StreamClient()
        : socket_fd_(-1),
          running_(false),
          last_completed_frame_sequence_(0),
          has_last_completed_frame_sequence_(false),
          next_jitter_buffer_sequence_(0),
          has_next_jitter_buffer_sequence_(false),
          previous_network_latency_ms_(0.0),
          has_previous_network_latency_(false) {
}

StreamClient::~StreamClient() {
    Close();
}

bool StreamClient::Connect(const ClientConfig &config, std::string *error_message) {
    Close();
    config_ = config;

    // 重置 UDP 相关状态
    ResetUdpState();

    // 重置 RTP 相关状态
    ResetRtpState();

    // 创建 jitter buffer 配置对象。
    // 这主要用于 UDP 模式，因为 UDP 到达时间可能忽快忽慢。
    AdaptiveJitterBufferConfig jitter_config;
    jitter_config.min_delay_ms = config_.udp_jitter_buffer_min_delay_ms;
    jitter_config.safety_factor = config_.udp_jitter_buffer_safety_factor;
    jitter_config.base_max_wait_ms = config_.udp_jitter_buffer_max_wait_ms;
    jitter_config.base_max_frames = config_.udp_jitter_buffer_max_frames;
    adaptive_jitter_.Configure(jitter_config);

    // 如果策略是 off，设置为 bypass 模式。
    // bypass 表示基本不做抖动缓冲，完成帧尽快输出。
    if (config_.udp_jitter_buffer_strategy == "off") {
        adaptive_jitter_.SetFixedMode(JitterBufferMode::kBypass);

    // low 表示低延迟模式。
    // 通常会少缓冲一些，优先降低延迟。
    } else if (config_.udp_jitter_buffer_strategy == "low") {
        adaptive_jitter_.SetFixedMode(JitterBufferMode::kLowLatency);

    // smooth 表示平滑模式。
    // 通常会多缓冲一点，换取播放更稳定。
    } else if (config_.udp_jitter_buffer_strategy == "smooth") {
        adaptive_jitter_.SetFixedMode(JitterBufferMode::kSmooth);
    } else if (config_.udp_jitter_buffer_strategy == "auto") {
        adaptive_jitter_.EnableAutoMode();
    }

    // 分配 UDP 接收缓冲区
    udp_datagram_buffer_.assign(config_.udp_max_datagram_size, 0);
    // 分配 RTP 接收缓冲区
    rtp_datagram_buffer_.assign(config_.udp_max_datagram_size, 0);

    const int socket_type = config_.transport == "tcp" ? SOCK_STREAM : SOCK_DGRAM;
    socket_fd_ = socket(AF_INET, socket_type, 0);
    if (socket_fd_ < 0) {
        if (error_message != nullptr) {
            *error_message = BuildSocketError("failed to create socket");
        }
        return false;
    }

    // 准备 IPv4 地址结构
    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_port = htons(static_cast<std::uint16_t>(config_.port));

    // 把字符串 IP 转成二进制 IP。
    if (inet_pton(AF_INET, config_.host.c_str(), &address.sin_addr) != 1) {
        if (error_message != nullptr) {
            *error_message = "invalid host: " + config_.host;
        }
        Close();
        return false;
    }

    // RTP 模式比较特殊。
    //
    // TCP/UDP 客户端是主动 connect 到服务端地址。
    // RTP 客户端在本项目里是“接收端”，需要 bind 到本地端口，
    // 等服务端把 RTP 包 sendto 到这个端口。
    if (config_.transport == "rtp") {
        if (bind(socket_fd_, reinterpret_cast<sockaddr *>(&address), sizeof(address)) != 0) {
            if (error_message != nullptr) {
                *error_message = BuildSocketError("failed to bind RTP socket");
            }
            Close();
            return false;
        }

    // 非 RTP，也就是 TCP 或自定义 UDP。
    //
    // TCP connect 会真正建立连接。
    // UDP connect 不建立真正连接，但会固定默认远端地址，
    // 之后可以直接用 send/recv，而不是每次 sendto/recvfrom 都传地址。
    } else if (connect(socket_fd_, reinterpret_cast<sockaddr *>(&address), sizeof(address)) != 0) {
        if (error_message != nullptr) {
            *error_message = BuildSocketError("failed to connect");
        }
        Close();
        return false;
    }

    // UDP/RTP 都是 datagram 接收，视频包可能很多，
    // 所以设置接收缓冲区，降低内核缓冲区太小导致丢包的概率
    if (config_.transport != "tcp") {
        setsockopt(
                socket_fd_,
                SOL_SOCKET,
                SO_RCVBUF,
                &config_.udp_receive_buffer_bytes,
                sizeof(config_.udp_receive_buffer_bytes));
    }

    {
        // 设置 socket 接收超时。
        // 这样 recv 不会永远阻塞，方便检测断开、退出或重连。
        timeval timeout{};

        // TCP 给 5 秒超时，因为 TCP 可能等待完整消息。
        // UDP/RTP 给 1 秒超时，因为它们是包式接收，超时后可以继续循环或返回错误。
        timeout.tv_sec = (config_.transport == "tcp") ? 5 : 1;
        timeout.tv_usec = 0;
        setsockopt(socket_fd_, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));
    }

    running_ = true;

    // RTP 不启动 keepalive。
    //
    // 原因：
    // 当前项目 RTP 是服务端按配置 remote_host/remote_port 单向发包，
    // 客户端只需要 bind 到本地端口接收。
    //
    // TCP/UDP 需要 keepalive：
    // - TCP keepalive 可以让服务端知道客户端还活着。
    // - UDP keepalive 更关键，服务端靠它知道客户端 IP:端口。
    if (config_.transport != "rtp") {
        // 先立即发送一次 keepalive。
        // UDP 模式下，如果不发 keepalive，服务端可能不知道客户端在哪里
        if (!SendKeepAlive()) {
            if (error_message != nullptr) {
                *error_message = BuildSocketError("failed to send initial keepalive");
            }
            Close();
            return false;
        }

        // 启动后台 keepalive 线程。
        // 它会按 config_.keepalive_interval_ms 周期性发送 keepalive
        keepalive_thread_ = std::thread(&StreamClient::KeepAliveLoop, this);
    }

    return true;
}

// 接收一帧完整视频数据
bool StreamClient::ReceiveFrame(ReceivedFrame *frame, std::string *error_message) {
    if (frame == nullptr || socket_fd_ < 0) {
        if (error_message != nullptr) {
            *error_message = "client is not connected";
        }
        return false;
    }

    // UDP 模式：
    // 内部会接收 UDP datagram，做分片重组、NACK、FEC、jitter buffer，
    // 最终输出完整 H.264 帧。
    if (config_.transport == "udp") {
        return ReceiveUdpFrame(frame, error_message);
    }

    // RTP 模式：
    // 内部会接收 RTP packet，解析 RTP header，
    // 处理单 NALU 或 FU-A 分片，重组 Annex-B H.264 帧。
    if (config_.transport == "rtp") {
        return ReceiveRtpFrame(frame, error_message);
    }

    // 默认 TCP 模式：
    // 内部会按 MessageHeader 读取完整帧。
    return ReceiveTcpFrame(frame, error_message);
}

// 获取 UDP 接收统计快照。
// 注意这里返回的是 snapshot，不是实时对象本身。
UdpReceiveStats StreamClient::udp_receive_stats() const {
    std::lock_guard<std::mutex> lock(udp_receive_stats_snapshot_mutex_);
    return udp_receive_stats_snapshot_;
}

// 获取当前 socket 绑定的本地端口
int StreamClient::BoundPort() const {
    if (socket_fd_ < 0) {
        return 0;
    }

    // 准备接收本地地址信息
    sockaddr_in address{};
    socklen_t address_length = sizeof(address);

    // getsockname 可以查询 socket 当前绑定的本地地址和端口
    if (getsockname(socket_fd_, reinterpret_cast<sockaddr *>(&address), &address_length) != 0) {
        return 0;
    }
    return static_cast<int>(ntohs(address.sin_port));
}

// 关闭客户端连接，释放 socket、线程和协议状态
void StreamClient::Close() {
    running_ = false;

    if (socket_fd_ >= 0) {
        shutdown(socket_fd_, SHUT_RDWR);
    }

    // 等待 keepalive 线程退出
    if (keepalive_thread_.joinable()) {
        keepalive_thread_.join();
    }

    // 真正关闭 socket fd
    if (socket_fd_ >= 0) {
        close(socket_fd_);
        socket_fd_ = -1;
    }

    // 清空 UDP 正在组装的帧
    udp_assemblies_.clear();

    // 清空 UDP jitter buffer 中已经完成但还没输出的帧
    udp_jitter_buffer_.clear();

    // 重置 UDP 状态和统计
    ResetUdpState();

    // 重置 RTP 组帧状态
    ResetRtpState();
}

// 发送 keepalive 包。
// TCP/UDP 都会用，RTP 当前不使用。
bool StreamClient::SendKeepAlive() {
    protocol::MessageHeader header{};
    protocol::FillMessageMagic(header.head_id);
    header.message_type = static_cast<std::uint16_t>(protocol::MessageType::kKeepAlive);

    // 如果不是 UDP，比如 TCP，
    // keepalive 只发一个 MessageHeader，不携带接收报告
    if (config_.transport != "udp") {
        header.payload_length = 0;
        std::lock_guard<std::mutex> lock(send_mutex_);
        return send(socket_fd_, &header, sizeof(header), MSG_NOSIGNAL) == static_cast<ssize_t>(sizeof(header));
    }

    // UDP 模式下，keepalive 会携带客户端接收统计报告。
    //
    // 服务端 UDP 后端没有 TCP 那样的连接，
    // 所以客户端周期性发送 keepalive：
    // 1. 告诉服务端“我还在线”
    // 2. 告诉服务端“我最近收包情况怎么样”
    const UdpReceiveStats stats = udp_receive_stats();
    protocol::UdpReceiverReport report{};
    report.report_timestamp_ns = MonotonicNowNs();
    report.datagrams_received = stats.datagrams_received;
    report.invalid_datagrams = stats.invalid_datagrams;
    report.fragments_received = stats.fragments_received;
    report.duplicate_fragments = stats.duplicate_fragments;
    report.timed_out_fragments = stats.timed_out_fragments;
    report.timed_out_frames = stats.timed_out_frames;
    report.completed_frames = stats.completed_frames;
    report.reordered_frames = stats.reordered_frames;
    report.jitter_samples = stats.jitter_samples;
    report.jitter_last_ms = stats.jitter_last_ms;
    report.jitter_avg_ms = stats.jitter_avg_ms;
    report.jitter_max_ms = stats.jitter_max_ms;

    // keepalive payload 是一个 UdpReceiverReport。
    header.payload_length = static_cast<std::uint32_t>(sizeof(report));

    // 创建完整发送缓冲：
    //
    // [MessageHeader][UdpReceiverReport]
    char payload[sizeof(header) + sizeof(report)];
    std::memcpy(payload, &header, sizeof(header));
    std::memcpy(payload + sizeof(header), &report, sizeof(report));
    std::lock_guard<std::mutex> lock(send_mutex_);
    return send(socket_fd_, payload, sizeof(payload), MSG_NOSIGNAL) == static_cast<ssize_t>(sizeof(payload));
}

// keepalive 后台线程函数。
// Connect() 成功后，TCP/UDP 模式会启动这个线程
void StreamClient::KeepAliveLoop() {
    while (running_.load()) {
        std::this_thread::sleep_for(std::chrono::milliseconds(config_.keepalive_interval_ms));

        // sleep 期间可能已经 Close()，所以醒来后再检查一次
        if (!running_.load()) {
            break;
        }

        // 发送 keepalive。
        // 如果发送失败，说明连接可能已经不可用，停止运行。
        if (!SendKeepAlive()) {
            running_ = false;
            break;
        }
    }
}

// 发布 UDP 接收统计快照。
// UDP 接收逻辑会更新 udp_receive_stats_，
// main.cpp 读取的是 udp_receive_stats_snapshot_。
void StreamClient::PublishUdpReceiveStatsSnapshot() {
    std::lock_guard<std::mutex> lock(udp_receive_stats_snapshot_mutex_);

    // 把实时统计复制为快照
    udp_receive_stats_snapshot_ = udp_receive_stats_;
}

}  // namespace sclient
