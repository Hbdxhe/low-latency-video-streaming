#include "modules/transport/tcp/TcpClientSession.h"

#include <arpa/inet.h>
#include <errno.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <sys/uio.h>
#include <unistd.h>

#include <chrono>
#include <cstring>

#include "common/log/Logger.h"
#include "common/net/StreamProtocol.h"
#include "common/time/MonotonicClock.h"

namespace sserver {
namespace modules {
namespace transport {
namespace tcp {

// TcpClientSession 表示一个已经连接的 TCP 客户端会话
TcpClientSession::TcpClientSession(
        int socket_fd,
        const config::TransportConfig &config,
        const std::shared_ptr<common::metrics::LatencyRecorder> &send_latency_recorder,
        int latency_log_interval_frames)
        : socket_fd_(socket_fd),
          config_(config),
          running_(false),
          send_latency_recorder_(send_latency_recorder),
          queue_wait_latency_recorder_(4096),
          send_time_latency_recorder_(4096),
          latency_log_interval_frames_(latency_log_interval_frames),
          sent_frames_(0),
          overflow_dropped_frames_(0),
          stale_dropped_frames_(0),
          dropped_incoming_non_keyframes_(0),
          backpressure_events_(0),
          max_queue_depth_(0),
          remote_endpoint_(BuildRemoteEndpoint()) {

    // 如果配置启用了 TCP_NODELAY，则关闭 Nagle 算法。
    //
    // 默认情况下，TCP 可能会把小包合并后再发送，以提高网络利用率。
    // 但实时音视频更关注低延迟，因此可以启用 TCP_NODELAY，让数据尽快发出去，减少小包等待合并带来的延迟。
    if (config_.enable_nodelay) {
        int flag = 1;
        setsockopt(socket_fd_, IPPROTO_TCP, TCP_NODELAY, &flag, sizeof(flag));
    }
}

TcpClientSession::~TcpClientSession() {
    Stop();
}

// 启动客户端会话。
//
// 一个客户端会话启动后会创建两个线程：
// 1. receive_thread_：用于接收客户端消息，主要用于 keepalive 和断连检测；
// 2. send_thread_：用于从发送队列中取帧，并写入 TCP socket。
bool TcpClientSession::Start() {
    if (socket_fd_ < 0) {
        return false;
    }
    running_ = true;
    receive_thread_ = std::thread(&TcpClientSession::ReceiveLoop, this);
    send_thread_ = std::thread(&TcpClientSession::SendLoop, this);
    return true;
}

// 停止客户端会话
void TcpClientSession::Stop() {
    bool expected = true;

    // compare_exchange_strong 的含义：
    // 如果 running_ 当前是 true，则将其改为 false，并返回 true；
    // 如果 running_ 当前不是 true，则返回 false。
    //
    // 这里无论是否成功，都调用 CloseSocket()，目的是保证 socket 一定会被关闭。
    if (!running_.compare_exchange_strong(expected, false)) {
        CloseSocket();
    } else {
        CloseSocket();
    }

    // 唤醒可能阻塞在发送队列上的发送线程
    outbound_frames_.NotifyAll();

    // 等待接收线程退出，需要判断 get_id()，避免线程在自己内部调用 Stop() 时 join 自己
    if (receive_thread_.joinable() && receive_thread_.get_id() != std::this_thread::get_id()) {
        receive_thread_.join();
    }

    // 等待发送线程退出
    if (send_thread_.joinable() && send_thread_.get_id() != std::this_thread::get_id()) {
        send_thread_.join();
    }

    // 清空尚未发送的帧，释放 shared_ptr 引用
    outbound_frames_.Clear();
}

// 判断当前会话是否仍在运行
bool TcpClientSession::IsRunning() const {
    return running_.load();
}

// 将一帧编码后的数据加入当前客户端的发送队列。
//
// 注意：这里不直接调用 send()。
// 原因是某个客户端网络变慢时，直接 send() 可能阻塞 Broadcast 调用链。
// 所以这里采用“生产者-消费者”模型：
//      EnqueueFrame() 只负责入队；
//      SendLoop() 负责真正发送。
void TcpClientSession::EnqueueFrame(common::model::EncodedFramePtr frame) {
    if (!running_.load() || !frame) {
        return;
    }

    QueuedFrame queued_frame;
    queued_frame.frame = frame;
    queued_frame.enqueue_timestamp_ns = common::time::MonotonicNowNs();

    // 如果当前队列已经达到最大积压帧数，说明客户端发送速度跟不上生产速度。
    // 这种现象称为 backpressure。
    const std::size_t queue_depth_before_push = outbound_frames_.Size();
    if (queue_depth_before_push >= config_.max_pending_frames) {
        ++backpressure_events_;
    }

    // 队列丢帧策略：
    // - drop_oldest_non_key: 优先丢弃非关键帧，保护关键帧以维持解码连续性
    // - drop_oldest: 直接丢弃队列最旧的帧
    std::size_t dropped = 0;

    if (config_.queue_drop_policy == "drop_oldest_non_key") {
        // 特殊情况：
        // 如果当前入队帧本身不是关键帧，队列已经满了，且队列里找不到可丢弃的非关键帧，那么说明队列里可能都是关键帧。
        // 此时为了保护关键帧，不把当前非关键帧放进去，直接丢弃当前帧。
        if (!frame->is_keyframe &&
            queue_depth_before_push >= config_.max_pending_frames &&
            !outbound_frames_.AnyMatching([](const QueuedFrame &candidate) {
                return candidate.frame != nullptr && !candidate.frame->is_keyframe;
            })) {
            ++dropped_incoming_non_keyframes_;
            return;
        }

        // 选择性入队：
        //
        // 如果队列超过 max_pending_frames，
        // 优先丢弃满足 lambda 条件的旧帧，也就是旧的非关键帧。
        dropped = outbound_frames_.PushDropSelective(
                queued_frame,
                config_.max_pending_frames,
                [](const QueuedFrame &candidate) {
                    return candidate.frame != nullptr && !candidate.frame->is_keyframe;
                });
    } else {
        // 默认策略：队列满时直接丢弃最旧帧。
        dropped = outbound_frames_.PushDropOldestCountDropped(queued_frame, config_.max_pending_frames);
    }

    // 累加由于队列溢出导致的丢帧数量
    overflow_dropped_frames_ += static_cast<std::uint64_t>(dropped);

    // 记录历史最大队列深度，用于观察客户端是否长期积压
    const std::size_t current_depth = outbound_frames_.Size();
    if (current_depth > max_queue_depth_) {
        max_queue_depth_ = current_depth;
    }
}

// 返回客户端远端地址字符串，例如 "192.168.1.10:52341"
std::string TcpClientSession::remote_endpoint() const {
    return remote_endpoint_;
}

// 接收线程主循环
void TcpClientSession::ReceiveLoop() {
    // keep_alive_budget 表示连续多少次超时后认为客户端失联。
    //
    // 当前 select 超时时间是 2 秒，预算是 5，所以大约 10 秒没有收到 keepalive，就认为客户端超时。
    int keep_alive_budget = 5;

    while (running_.load()) {
        fd_set read_fds;
        FD_ZERO(&read_fds);
        FD_SET(socket_fd_, &read_fds);

        // select 最多等待 2 秒
        timeval timeout{};
        timeout.tv_sec = 2;
        timeout.tv_usec = 0;

        // 等待 socket 可读。
        //
        // ready > 0：socket 有数据可读；
        // ready = 0：超时；
        // ready < 0：出错。
        const int ready = select(socket_fd_ + 1, &read_fds, nullptr, nullptr, &timeout);
        if (!running_.load()) {
            break;
        }

        if (ready == 0) {
            // 超时未收到数据，减少 keepalive 预算
            --keep_alive_budget;

            if (keep_alive_budget <= 0) {
                common::log::Logger::Warn("client keepalive timeout: " + remote_endpoint_);
                break;
            }
            continue;
        }

        if (ready < 0) {
            // select 被信号中断，可以重试
            if (errno == EINTR) {
                continue;
            }
            common::log::Logger::Warn("client select failed: " + remote_endpoint_);
            break;
        }

        // socket 可读，先读取一个固定长度消息头
        common::net::MessageHeader header{};
        if (!ReceiveAll(reinterpret_cast<char *>(&header), sizeof(header))) {
            break;
        }

        // 如果收到的是合法 keepalive 消息，则恢复预算
        if (common::net::HasValidMessageMagic(header) &&
            header.message_type == static_cast<std::uint16_t>(common::net::MessageType::kKeepAlive)) {
            keep_alive_budget = 5;
        }
    }

    // 走到这里表示接收线程认为连接应该关闭
    running_ = false;

    // 关闭 socket，让发送线程中的 sendmsg 也尽快失败退出
    CloseSocket();

    // 唤醒发送线程
    outbound_frames_.NotifyAll();
}

// 发送线程主循环
void TcpClientSession::SendLoop() {
    while (running_.load()) {
        QueuedFrame queued_frame;

        // 等待队列中出现待发送帧。
        //
        // 超时时间设置为 2ms，是为了让线程能周期性检查 running_。
        if (!outbound_frames_.WaitPopFor(&queued_frame, std::chrono::milliseconds(2))) {
            continue;
        }

        const std::uint64_t send_start_timestamp_ns = common::time::MonotonicNowNs();

        // 统计队列等待时间
        if (queued_frame.enqueue_timestamp_ns != 0 && send_start_timestamp_ns >= queued_frame.enqueue_timestamp_ns) {
            const std::uint64_t queue_wait_ns = send_start_timestamp_ns - queued_frame.enqueue_timestamp_ns;
            queue_wait_latency_recorder_.RecordNs(queue_wait_ns);

            // 如果非关键帧在队列里等待太久，说明它已经过期。
            // 实时视频中，过旧的非关键帧继续发送意义不大，反而会增加延迟，因此直接丢弃。
            // 关键帧不在这里丢弃，因为关键帧对解码恢复更重要。
            if (config_.max_queue_wait_ms > 0 &&
                queued_frame.frame != nullptr &&
                !queued_frame.frame->is_keyframe &&
                queue_wait_ns > static_cast<std::uint64_t>(config_.max_queue_wait_ms) * 1000ULL * 1000ULL) {
                ++stale_dropped_frames_;
                continue;
            }
        }

        // 真正发送当前帧
        if (!SendFrame(queued_frame.frame, send_start_timestamp_ns)) {
            break;
        }

        // 记录 socket 发送耗时
        const std::uint64_t send_end_timestamp_ns = common::time::MonotonicNowNs();
        if (send_end_timestamp_ns >= send_start_timestamp_ns) {
            send_time_latency_recorder_.RecordNs(send_end_timestamp_ns - send_start_timestamp_ns);
        }

        // 统计从采集到发送完成的端到端延迟
        if (send_latency_recorder_ != nullptr && queued_frame.frame->capture_timestamp_ns != 0) {
            send_latency_recorder_->RecordNs(send_end_timestamp_ns - queued_frame.frame->capture_timestamp_ns);

            ++sent_frames_;

            // 每隔 latency_log_interval_frames_ 帧打印一次统计信息
            if (latency_log_interval_frames_ > 0 &&
                sent_frames_ % static_cast<std::uint64_t>(latency_log_interval_frames_) == 0) {
                common::log::Logger::Info(
                        "transport latency " + remote_endpoint_ + " " + send_latency_recorder_->Format("capture_to_send"));
                common::log::Logger::Info(
                        "tcp queue stats " + remote_endpoint_ +
                        " current_depth=" + std::to_string(outbound_frames_.Size()) +
                        " max_depth=" + std::to_string(max_queue_depth_) +
                        " overflow_dropped_frames=" + std::to_string(overflow_dropped_frames_) +
                        " stale_dropped_frames=" + std::to_string(stale_dropped_frames_) +
                        " dropped_incoming_non_keyframes=" + std::to_string(dropped_incoming_non_keyframes_) +
                        " backpressure_events=" + std::to_string(backpressure_events_) +
                        " max_queue_wait_ms=" + std::to_string(config_.max_queue_wait_ms) +
                        " queue_drop_policy=" + config_.queue_drop_policy +
                        " " + queue_wait_latency_recorder_.Format("queue_wait") +
                        " " + send_time_latency_recorder_.Format("send_time"));
            }
        }
    }

    // 发送线程退出时关闭会话
    running_ = false;
    CloseSocket();
    outbound_frames_.NotifyAll();
}

// 从 TCP socket 中读取指定长度的数据。
//
// TCP 是字节流协议，一次 recv() 不一定能读满 length 字节。
// 所以这里需要循环 recv()，直到读满 length，或者连接断开/出错。
bool TcpClientSession::ReceiveAll(char *buffer, std::size_t length) {
    std::lock_guard<std::mutex> lock(receive_mutex_);

    std::size_t received = 0;

    while (received < length && running_.load()) {
        const ssize_t result = recv(socket_fd_, buffer + received, length - received, 0);

        if (result < 0) {
            // recv 被信号中断，可以继续读取
            if (errno == EINTR) {
                continue;
            }
            return false;
        }

        // result == 0 表示对端已经正常关闭连接
        if (result == 0) {
            return false;
        }

        received += static_cast<std::size_t>(result);
    }
    return received == length;
}

// 发送一帧 EncodedFrame。
//
// 发送格式大致为：
//      MessageHeader + 可选 FrameDiagnosticMetadata + H.264 payload
bool TcpClientSession::SendFrame(common::model::EncodedFramePtr frame, std::uint64_t send_start_timestamp_ns) {
    common::net::MessageHeader header{};

    // 填充协议 magic，用于接收端识别这是本项目定义的消息
    common::net::FillMessageMagic(header.head_id);

    // 消息类型：音视频流
    header.message_type = static_cast<std::uint16_t>(common::net::MessageType::kAvStream);

    // 子类型：视频帧或音频帧。
    header.sub_type = static_cast<std::uint16_t>(frame->type);

    common::net::FrameDiagnosticMetadata metadata{};
    if (config_.embed_frame_metadata) {
        // 如果开启元数据嵌入，则在 payload 前插入诊断元信息。
        //
        // 这些字段用于端到端延迟分析：
        //      capture_timestamp_ns：采集时间；
        //      encode_start_timestamp_ns：编码开始时间；
        //      encode_end_timestamp_ns：编码结束时间；
        //      transport_send_timestamp_ns：传输发送时间。
        metadata.sequence = frame->sequence;
        metadata.capture_timestamp_ns = frame->capture_timestamp_ns;
        metadata.encode_start_timestamp_ns = frame->encode_start_timestamp_ns;
        metadata.encode_end_timestamp_ns = frame->encode_end_timestamp_ns;
        metadata.transport_send_timestamp_ns = send_start_timestamp_ns;

        header.payload_length = static_cast<std::uint32_t>(sizeof(metadata) + frame->payload.size());
    } else {
        header.payload_length = static_cast<std::uint32_t>(frame->payload.size());
    }

    // metadata 指针。如果不发送 metadata，则传 nullptr 和长度 0。
    const char *metadata_ptr = config_.embed_frame_metadata
            ? reinterpret_cast<const char *>(&metadata)
            : nullptr;
    const std::size_t metadata_length = config_.embed_frame_metadata ? sizeof(metadata) : 0;

    // H.264 payload 指针。如果 payload 为空，则传 nullptr，避免访问空 vector 的 data。
    const char *payload_ptr = frame->payload.empty()
            ? nullptr
            : reinterpret_cast<const char *>(frame->payload.data());

    // 一次逻辑消息分为 header、metadata、payload 三部分发送
    return SendMessageParts(
            reinterpret_cast<const char *>(&header),
            sizeof(header),
            metadata_ptr,
            metadata_length,
            payload_ptr,
            frame->payload.size());
}

// 发送一个由多段内存组成的消息。
//
// 这里使用 sendmsg + iovec，原因是： header、metadata、payload 本来就在不同内存区域。
// 使用 writev/sendmsg 可以避免先把三段数据拷贝到一个大 buffer 中。
bool TcpClientSession::SendMessageParts(
        const char *header,
        std::size_t header_length,
        const char *metadata,
        std::size_t metadata_length,
        const char *payload,
        std::size_t payload_length) {
    std::lock_guard<std::mutex> lock(send_mutex_);

    // 三段消息长度
    iovec iovecs[3];

    // 三段消息起始地址
    std::size_t lengths[3] = {header_length, metadata_length, payload_length};

    // 三段消息起始地址
    const char *buffers[3] = {header, metadata, payload};

    // 实际需要发送的段数
    std::size_t active_count = 0;

    for (std::size_t index = 0; index < 3; ++index) {
        if (buffers[index] != nullptr && lengths[index] > 0) {
            iovecs[active_count].iov_base = const_cast<char *>(buffers[index]);
            iovecs[active_count].iov_len = lengths[index];
            ++active_count;
        }
    }

    // 循环发送，直到所有 iovec 都发送完，或者连接异常
    while (active_count > 0 && running_.load()) {
        msghdr message{};
        message.msg_iov = iovecs;
        message.msg_iovlen = active_count;

        // sendmsg 可能只发送一部分数据。
        // MSG_NOSIGNAL 表示当对端关闭连接时，不产生 SIGPIPE 信号，而是让 sendmsg 返回错误，避免进程被信号终止。
        const ssize_t result = sendmsg(socket_fd_, &message, MSG_NOSIGNAL);

        if (result < 0) {
            // 被信号中断，重试
            if (errno == EINTR) {
                continue;
            }
            return false;
        }

        // 正常情况下 sendmsg 返回 0 比较少见。
        // 这里把它视为连接不可继续发送。
        if (result == 0) {
            return false;
        }

        // 本次实际发送的字节数
        std::size_t sent = static_cast<std::size_t>(result);

        // 本次实际发送的字节数
        std::size_t shift = 0;

        // 如果 sent 覆盖了若干完整 iovec，则跳过这些已经发完的段。
        while (shift < active_count && sent >= iovecs[shift].iov_len) {
            sent -= iovecs[shift].iov_len;
            ++shift;
        }

        // 删除已经完整发送的 iovec 段。
        //
        // 例如原来有 [header, metadata, payload]，
        // 如果 header 已经完整发送，则数组前移为 [metadata, payload]。
        if (shift > 0) {
            for (std::size_t index = shift; index < active_count; ++index) {
                iovecs[index - shift] = iovecs[index];
            }
            active_count -= shift;
        }

        // 如果当前第一个 iovec 只发送了一部分，则移动它的起始指针，并减少剩余长度。
        //
        // 例如 payload 还有 1000 字节，本次发了 200 字节，
        // 那么下一次应该从 payload + 200 开始继续发 800 字节。
        if (active_count > 0 && sent > 0) {
            iovecs[0].iov_base = static_cast<char *>(iovecs[0].iov_base) + sent;
            iovecs[0].iov_len -= sent;
        }
    }

    // active_count == 0 表示所有段都已经发送完成。
    return active_count == 0;
}

// 关闭客户端 socket。
//
// shutdown(SHUT_RDWR) 会关闭读写方向，可以让阻塞在 recv/send/select 上的线程尽快返回。
// close() 释放文件描述符。
void TcpClientSession::CloseSocket() {
    if (socket_fd_ >= 0) {
        shutdown(socket_fd_, SHUT_RDWR);
        close(socket_fd_);
        socket_fd_ = -1;
    }
}

// 构造客户端远端地址字符串。
//
// getpeername() 可以通过已连接 socket 获取对端 IP 和端口。
// 返回格式类似：
//      "192.168.1.10:52341"
std::string TcpClientSession::BuildRemoteEndpoint() const {
    sockaddr_in address{};
    socklen_t address_length = sizeof(address);
    if (getpeername(socket_fd_, reinterpret_cast<sockaddr *>(&address), &address_length) != 0) {
        return "unknown-client";
    }

    char ip_buffer[INET_ADDRSTRLEN] = {0};

    // 将网络字节序 IPv4 地址转换成点分十进制字符串。
    inet_ntop(AF_INET, &address.sin_addr, ip_buffer, sizeof(ip_buffer));

    // ntohs() 将网络字节序端口转换为主机字节序整数
    return std::string(ip_buffer) + ":" + std::to_string(ntohs(address.sin_port));
}

}  // namespace tcp
}  // namespace transport
}  // namespace modules
}  // namespace sserver
