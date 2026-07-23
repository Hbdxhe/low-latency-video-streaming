#include "modules/transport/udp/UdpStreamingBackend.h"

#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <unistd.h>

#include <algorithm>
#include <chrono>
#include <cstring>

#include "common/log/Logger.h"
#include "common/net/StreamProtocol.h"
#include "common/time/MonotonicClock.h"

// 这个文件实现的是“UDP 音视频流发送后端”。它不是编码器，也不是播放器。
// 它的职责可以理解为：
//
//     已编码好的音/视频帧 EncodedFrame
//              ↓
//     按 UDP 包大小切成多个 fragment 分片
//              ↓
//     给每个分片加协议头 MessageHeader + UdpFrameFragmentHeader
//              ↓
//     用 sendto() / sendmmsg() 发给每个客户端
//              ↓
//     如果客户端发现丢包，可以通过 NACK 请求重传
//
// 初学者需要先理解几个词：
// 1. EncodedFrame：已经被编码器压缩过的一帧数据，例如 H.264/H.265 视频帧、AAC/Opus 音频帧。
//    这里不会关心画面内容是什么，只把 payload 当成一段二进制字节发送。
// 2. UDP datagram：一次 UDP 发送出去的网络包。UDP 包太大会容易被 IP 层分片甚至丢弃，
//    所以大帧需要拆成多个较小的 UDP 分片。
// 3. fragment：一帧数据被拆出来的一小片。接收端需要根据 frame_sequence、fragment_index、
//    fragment_count 等字段把它们重新拼回完整帧。
// 4. keepalive：客户端定期发来的“我还在”的小包。UDP 没有连接，所以服务端只能靠 keepalive
//    判断哪些客户端仍然在线。
// 5. NACK：Negative ACK。客户端发现某些分片没收到，就告诉服务端“第几帧的第几个分片丢了”，
//    服务端再从缓存里找出来重发。
// 6. FEC：Forward Error Correction，前向纠错。这里用的是简单 XOR 奇偶校验。
//    如果一组数据分片只丢了一个，接收端可能不需要等重传，直接用校验分片恢复。

namespace sserver {
namespace modules {
namespace transport {
namespace udp {

namespace {

// 将 source 数据异或累加到 target 中，用于 FEC 前向纠错。
//
// XOR FEC 的基本思想：
//      parity = fragment0 ^ fragment1 ^ fragment2 ^ ...
//
// 如果接收端只丢失一个数据分片，就可以用：
//      lost = parity ^ 所有已收到的数据分片
//
// 恢复出丢失的分片。
void XorInto(std::vector<std::uint8_t> *target, const std::uint8_t *source, std::size_t length) {
    if (target == nullptr || source == nullptr || length == 0) {
        return;
    }

    // target 不够长时扩容，并用 0 填充
    // 因为 x ^ 0 = x，所以初始值为 0 不影响异或结果
    if (target->size() < length) {
        target->resize(length, 0);
    }

    // 对每个字节做异或累加
    for (std::size_t index = 0; index < length; ++index) {
        (*target)[index] ^= source[index];
    }
}

// 判断收到的 UDP 包是不是 keepalive 包。
// keepalive 包的作用：客户端周期性告诉服务端“我还活着，请继续给我发流”。
bool IsKeepAlivePacket(const char *buffer, std::size_t length) {
    // 如果收到的数据长度连 MessageHeader 都不够，那肯定不是一个合法协议包。
    if (length < sizeof(common::net::MessageHeader)) {
        return false;
    }

    // 把原始字节 buffer 当成 MessageHeader 来看
    const common::net::MessageHeader *header = reinterpret_cast<const common::net::MessageHeader *>(buffer);

    // 判断两件事：
    // 1. magic 是否正确，也就是是不是本项目协议包
    // 2. message_type 是否是 kKeepAlive
    return common::net::HasValidMessageMagic(*header) &&
           header->message_type == static_cast<std::uint16_t>(common::net::MessageType::kKeepAlive);
}

// 判断收到的 UDP 包是不是 NACK 包
bool IsNackPacket(const char *buffer, std::size_t length) {
    if (length < sizeof(common::net::MessageHeader)) {
        return false;
    }

    const common::net::MessageHeader *header = reinterpret_cast<const common::net::MessageHeader *>(buffer);
    return common::net::HasValidMessageMagic(*header) &&
           header->message_type == static_cast<std::uint16_t>(common::net::MessageType::kUdpNack);
}

// 解析 keepalive 包里的 UdpReceiverReport
bool ParseKeepAliveReport(
        const char *buffer,
        std::size_t length,
        common::net::UdpReceiverReport *report) {

    // 如果 report 是空指针，无法写入解析结果，返回 false
    // 如果 length 连 MessageHeader 都不够，也返回 false
    if (report == nullptr || length < sizeof(common::net::MessageHeader)) {
        return false;
    }

    // 解析通用消息头
    const common::net::MessageHeader *header = reinterpret_cast<const common::net::MessageHeader *>(buffer);

    // 做一组合法性检查
    if (!common::net::HasValidMessageMagic(*header) ||
        // 必须是 keepalive 类型
        header->message_type != static_cast<std::uint16_t>(common::net::MessageType::kKeepAlive) ||
        // payload_length 必须至少能放下一个 UdpReceiverReport
        header->payload_length < sizeof(common::net::UdpReceiverReport) ||
        // 实际收到的包长度也必须够
        length < sizeof(common::net::MessageHeader) + sizeof(common::net::UdpReceiverReport)) {
        return false;
    }

    // 从 buffer 中拷贝 UdpReceiverReport 到 report。
    //
    // buffer 布局是：[MessageHeader][UdpReceiverReport]
    // 所以 report 的起始位置是： buffer + sizeof(MessageHeader)
    std::memcpy(
            report,
            buffer + sizeof(common::net::MessageHeader),
            sizeof(common::net::UdpReceiverReport));
    return true;
}


// 解析客户端发来的 NACK 请求。
// NACK 请求告诉服务端：哪些帧的哪些 fragment 丢了。
bool ParseNackRequest(
        const char *buffer,
        std::size_t length,
        common::net::UdpNackHeader *nack_header,                // 输出：NACK 总头
        std::vector<common::net::UdpNackItem> *nack_items) {    // 输出：丢失分片列表

    if (nack_header == nullptr || nack_items == nullptr || length < sizeof(common::net::MessageHeader)) {
        return false;
    }

    const common::net::MessageHeader *header = reinterpret_cast<const common::net::MessageHeader *>(buffer);

    if (!common::net::HasValidMessageMagic(*header) ||
        header->message_type != static_cast<std::uint16_t>(common::net::MessageType::kUdpNack) ||
        header->payload_length < sizeof(common::net::UdpNackHeader) ||
        length < sizeof(common::net::MessageHeader) + sizeof(common::net::UdpNackHeader)) {
        return false;
    }

    // 拷贝 NACK 头。
    // buffer 布局：[MessageHeader][UdpNackHeader][UdpNackItem...]
    std::memcpy(
            nack_header,
            buffer + sizeof(common::net::MessageHeader),
            sizeof(common::net::UdpNackHeader));

    // 根据 nack_header->request_count 计算理论 payload 长度。
    //
    // 举例：request_count = 3
    // payload 应该是：UdpNackHeader + 3 个 UdpNackItem
    const std::size_t expected_payload_length =
            sizeof(common::net::UdpNackHeader) +
            static_cast<std::size_t>(nack_header->request_count) * sizeof(common::net::UdpNackItem);

    // 检查 MessageHeader 里写的 payload_length 是否和实际请求数量匹配。
    // 也检查实际收到的 UDP 包长度是否足够。
    if (header->payload_length != expected_payload_length ||
        length < sizeof(common::net::MessageHeader) + expected_payload_length) {
        return false;
    }

    // 按请求数量调整 nack_items 的大小
    nack_items->resize(nack_header->request_count);

    // 如果确实有 NACK item，就把它们从 buffer 里拷贝出来
    if (!nack_items->empty()) {
        std::memcpy(
                nack_items->data(),
                buffer + sizeof(common::net::MessageHeader) + sizeof(common::net::UdpNackHeader),
                nack_items->size() * sizeof(common::net::UdpNackItem));
    }
    return true;
}

}  // namespace


// UdpStreamingBackend 的构造函数。
// 参数 send_latency_recorder 用来记录“从采集到发送”的延迟。
UdpStreamingBackend::UdpStreamingBackend(
        const std::shared_ptr<common::metrics::LatencyRecorder> &send_latency_recorder)
        : backend_name_("udp"),
          latency_log_interval_frames_(120),    // 每隔多少帧打印一次延迟统计
          socket_fd_(-1),
          bound_port_(0),
          running_(false),
          send_latency_recorder_(send_latency_recorder),
          state_(core::ModuleState::kCreated),
          sent_frames_(0),                      // 已成功发送的帧数
          dropped_fragmented_frames_(0),        // 因为拆片或发送失败而丢弃的帧数
          sent_fragments_(0),                   // 已发送的 UDP fragment 数量
          fec_fragments_sent_(0),               // 已发送的 FEC 校验分片数量
          failed_fragments_(0),                 // 发送失败的 fragment 数量
          nack_requests_received_(0),           // 收到的 NACK 请求次数
          nack_fragments_requested_(0),         // 客户端总共请求重传的 fragment 数量
          retransmitted_fragments_sent_(0),     // 服务端成功重传出去的 fragment 数量
          retransmit_fragment_misses_(0),       // 客户端请求重传，但服务端缓存里没找到的 fragment 数量
          retransmit_fragments_throttled_(0) {  // 因为超过单次重传上限而被限制的 fragment 数量

}

UdpStreamingBackend::~UdpStreamingBackend() {
    shutdown();
}

bool UdpStreamingBackend::initialize(const core::ApplicationContext &context) {
    config_ = context.config.transport;
    latency_log_interval_frames_ = context.config.runtime.latency_log_interval_frames;
    state_ = core::ModuleState::kInitialized;
    return true;
}

bool UdpStreamingBackend::start() {
    // 如果配置里传输模块没有启用，那这里不打开 socket，但仍然把状态置为 running。
    if (!config_.enabled) {
        state_ = core::ModuleState::kRunning;
        return true;
    }

    // 打开 UDP socket，并 bind 到指定地址和端口
    if (!OpenSocket()) {
        state_ = core::ModuleState::kFailed;
        return false;
    }

    running_ = true;

    // 启动后台线程
    receive_thread_ = std::thread(&UdpStreamingBackend::ReceiveLoop, this);
    state_ = core::ModuleState::kRunning;
    return true;
}

// 停止 UDP 后端
void UdpStreamingBackend::stop() {
    // 如果当前状态不是 running，就不需要重复停止
    if (state_.load() != core::ModuleState::kRunning) {
        return;
    }

    running_ = false;
    CloseSocket();

    if (receive_thread_.joinable()) {
        receive_thread_.join();
    }

    {
        std::lock_guard<std::mutex> lock(clients_mutex_);
        // 清空客户端列表，因为 UDP 后端停止了，就不再维护任何客户端。
        clients_.clear();
    }
    {
        std::lock_guard<std::mutex> lock(retransmit_cache_mutex_);
        // 清空重传缓存，这些缓存的 fragment 只在当前 UDP 会话里有意义。
        retransmit_cache_.clear();
    }

    state_ = core::ModuleState::kStopped;
}

// 关闭 UDP 后端
void UdpStreamingBackend::shutdown() {
    stop();
    state_ = core::ModuleState::kShutdown;
}

// 返回当前模块状态
core::ModuleState UdpStreamingBackend::state() const {
    return state_.load();
}

// 发送一帧 EncodedFrame。
// 这是上游编码模块把一帧交给 UDP 后端的入口
void UdpStreamingBackend::Broadcast(common::model::EncodedFramePtr frame) {

    if (!config_.enabled || !frame || socket_fd_ < 0) {
        return;
    }

    const std::uint64_t now_ns = common::time::MonotonicNowNs();
    // 获取当前在线客户端地址快照。
    //
    // UDP 没有连接，所以 clients_ 是靠客户端 keepalive 维护出来的。
    // SnapshotClients() 会：
    // - 清理超时客户端
    // - 把仍然在线的客户端地址复制出来
    const std::vector<sockaddr_in> clients = SnapshotClients(now_ns);
    if (clients.empty()) {
        return;
    }

    // 真正执行“拆片 + 发送”的函数。
    //
    // SendFrameFragments() 会把 frame->payload 拆成多个 UDP fragment，
    // 然后发给 clients 里的每一个客户端。
    if (!SendFrameFragments(frame, clients)) {
        // 如果拆片或发送失败，统计丢弃帧数
        ++dropped_fragmented_frames_;

        // 第一次失败时打印日志。
        // 后续每 30 次失败打印一次，避免日志刷屏。
        if (dropped_fragmented_frames_ == 1 || dropped_fragmented_frames_ % 30 == 0) {
            common::log::Logger::Warn("udp frame dropped because fragmentation or send failed");
        }
        return;
    }

    // 如果配置了延迟记录器，并且这一帧有采集时间戳，就记录“采集到发送”的耗时。
    if (send_latency_recorder_ != nullptr && frame->capture_timestamp_ns != 0) {

        send_latency_recorder_->RecordNs(common::time::MonotonicNowNs() - frame->capture_timestamp_ns);

        // 成功发送帧数 +1
        ++sent_frames_;

        if (latency_log_interval_frames_ > 0 &&
            sent_frames_ % static_cast<std::uint64_t>(latency_log_interval_frames_) == 0) {
            common::log::Logger::Info("transport latency udp-broadcast " +
                                      send_latency_recorder_->Format("capture_to_send"));

            // 打印 UDP 传输统计
            common::log::Logger::Info(
                    "udp transport stats"
                    " sent_frames=" + std::to_string(sent_frames_) +
                    " sent_fragments=" + std::to_string(sent_fragments_) +
                    " fec_fragments_sent=" + std::to_string(fec_fragments_sent_) +
                    " failed_fragments=" + std::to_string(failed_fragments_) +
                    " dropped_frames=" + std::to_string(dropped_fragmented_frames_) +
                    " nack_requests_received=" + std::to_string(nack_requests_received_) +
                    " nack_fragments_requested=" + std::to_string(nack_fragments_requested_) +
                    " retransmitted_fragments_sent=" + std::to_string(retransmitted_fragments_sent_) +
                    " retransmit_fragment_misses=" + std::to_string(retransmit_fragment_misses_) +
                    " retransmit_fragments_throttled=" + std::to_string(retransmit_fragments_throttled_));

            std::lock_guard<std::mutex> lock(clients_mutex_);
            for (std::size_t index = 0; index < clients_.size(); ++index) {
                // 如果这个客户端曾经上报过接收统计，就打印出来
                if (clients_[index].has_report) {
                    common::log::Logger::Info(FormatClientReport(clients_[index]));
                }
            }
        }
    }
}

// 返回当前 UDP 后端实际绑定的端口
int UdpStreamingBackend::bound_port() const {
    return bound_port_;
}

// 返回当前传输后端类型
TransportBackend UdpStreamingBackend::backend() const {
    return TransportBackend::kUdp;
}

// 返回当前后端名字
const std::string &UdpStreamingBackend::backend_name() const {
    return backend_name_;
}

// 打开 UDP socket，并绑定到指定地址和端口
bool UdpStreamingBackend::OpenSocket() {
    // 创建一个 UDP socket。
    //
    // AF_INET：使用 IPv4。
    // SOCK_DGRAM：使用 UDP 数据报。
    // 0：使用默认协议，也就是 UDP。
    socket_fd_ = socket(AF_INET, SOCK_DGRAM, 0);
    if (socket_fd_ < 0) {
        common::log::Logger::Error("failed to create udp transport socket");
        return false;
    }

    int reuse = 1;
    // 设置 SO_REUSEADDR。
    //
    // 作用：
    // 程序刚退出又马上重启时，端口可能还处于系统保留状态。
    // 开启这个选项后，端口更容易被重新绑定。
    setsockopt(socket_fd_, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));

    // 设置 UDP 接收缓冲区大小。
    //
    // 这个 socket 不仅用来发视频，也用来收客户端控制包：
    // - keepalive
    // - NACK
    //
    // 接收缓冲区太小，控制包也可能丢。
    setsockopt(socket_fd_, SOL_SOCKET, SO_RCVBUF, &config_.udp_receive_buffer_bytes, sizeof(config_.udp_receive_buffer_bytes));

    // 设置 UDP 发送缓冲区大小。
    //
    // 视频帧会被拆成很多 UDP fragment。
    // 如果发送缓冲区太小，发送高码率视频时更容易失败或抖动
    setsockopt(socket_fd_, SOL_SOCKET, SO_SNDBUF, &config_.udp_send_buffer_bytes, sizeof(config_.udp_send_buffer_bytes));

    // 把 socket 设置为非阻塞
    const int current_flags = fcntl(socket_fd_, F_GETFL, 0);
    if (current_flags >= 0) {
        fcntl(socket_fd_, F_SETFL, current_flags | O_NONBLOCK);
    }

    // sockaddr_in 是 IPv4 地址结构体
    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_port = htons(static_cast<std::uint16_t>(config_.listen_port));
    address.sin_addr.s_addr = inet_addr(config_.bind_address.c_str());

    // 把 socket 绑定到 address 指定的 IP:端口
    if (bind(socket_fd_, reinterpret_cast<sockaddr *>(&address), sizeof(address)) < 0) {
        common::log::Logger::Error("failed to bind udp transport socket");
        CloseSocket();
        return false;
    }

    // 如果 listen_port 配置为 0，表示让操作系统自动选择一个空闲端口。
    if (config_.listen_port == 0) {
        // 用于接收 getsockname 查询出来的实际绑定地址
        sockaddr_in bound_address{};
        socklen_t bound_length = sizeof(bound_address);

        // getsockname 可以查询 socket 实际绑定到了哪个 IP:端口。
        if (getsockname(socket_fd_, reinterpret_cast<sockaddr *>(&bound_address), &bound_length) == 0) {
            bound_port_ = ntohs(bound_address.sin_port);
            common::log::Logger::Info("udp transport bound to ephemeral port " + std::to_string(bound_port_));
        }
    } else {
        bound_port_ = config_.listen_port;
    }

    return true;
}

// 关闭 UDP socket
void UdpStreamingBackend::CloseSocket() {
    if (socket_fd_ >= 0) {
        close(socket_fd_);
        socket_fd_ = -1;
    }
}

void UdpStreamingBackend::ReceiveLoop() {
    // 创建接收缓冲区。
    // 每次 recvfrom 最多接收 udp_max_datagram_size 字节。
    std::vector<char> buffer(config_.udp_max_datagram_size);

    while (running_.load()) {
        // fd_set 是 select 使用的 fd 集合。
        fd_set read_fds;
        FD_ZERO(&read_fds);
        FD_SET(socket_fd_, &read_fds);

        // 设置 select 超时时间
        timeval timeout{};
        timeout.tv_sec = 0;
        timeout.tv_usec = 200 * 1000;

        // 等待 socket 可读，最多等 200ms。
        //
        // 返回值 ready：
        // - ready > 0：有 fd 可读
        // - ready == 0：超时，没有收到数据
        // - ready < 0：select 出错
        const int ready = select(socket_fd_ + 1, &read_fds, nullptr, nullptr, &timeout);
        if (!running_.load()) {
            break;
        }

        // ready == 0 表示 200ms 内没有收到任何 UDP 包
        if (ready == 0) {
            std::lock_guard<std::mutex> lock(clients_mutex_);
            // 清理超时客户端。
            //
            // 因为客户端如果还活着，会定期发 keepalive。
            // 很久没发 keepalive，就认为它离线。
            PruneStaleClientsLocked(common::time::MonotonicNowNs());
            continue;
        }

        if (ready < 0) {
            if (errno == EINTR) {
                continue;
            }
            common::log::Logger::Warn("udp transport select failed");
            break;
        }

        // 走到这里说明 socket 可读，可以 recvfrom。
        // client_address 用来保存“是谁发来的 UDP 包”。
        sockaddr_in client_address{};
        socklen_t client_length = sizeof(client_address);

        // 从 UDP socket 接收数据。
        //
        // recvfrom 和 recv 的区别：
        // recv 只收数据。
        // recvfrom 不仅收数据，还能拿到发送方地址。
        //
        // 对 UDP 来说，发送方地址非常重要。
        // 因为 UDP 没有连接，服务端就是通过这个地址知道客户端在哪里。
        const ssize_t received = recvfrom(
                socket_fd_,
                buffer.data(),
                buffer.size(),
                0,
                reinterpret_cast<sockaddr *>(&client_address),
                &client_length);


        if (received < 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR) {
                continue;
            }
            common::log::Logger::Warn("udp transport recvfrom failed");
            break;
        }

        const std::uint64_t now_ns = common::time::MonotonicNowNs();

        if (IsKeepAlivePacket(buffer.data(), static_cast<std::size_t>(received))) {
            // 准备一个接收端报告结构体。
            //
            // 如果 keepalive 包里携带了 UdpReceiverReport，ParseKeepAliveReport 会把内容写到 report 里。
            common::net::UdpReceiverReport report{};

            // 尝试解析 keepalive 里的客户端接收报告。
            //
            // has_report 为 true 表示：这个 keepalive 不只是心跳，还带了统计数据。
            const bool has_report = ParseKeepAliveReport(buffer.data(), static_cast<std::size_t>(received), &report);
            RegisterClient(client_address, now_ns, has_report ? &report : nullptr);

        } else if (IsNackPacket(buffer.data(), static_cast<std::size_t>(received))) {
            // NACK 总头
            common::net::UdpNackHeader nack_header{};
            std::vector<common::net::UdpNackItem> nack_items;

            // 解析 NACK 请求。
            if (ParseNackRequest(buffer.data(), static_cast<std::size_t>(received), &nack_header, &nack_items)) {
                // 收到 NACK 也说明这个客户端还活着，所以先注册或刷新客户端。
                RegisterClient(client_address, now_ns, nullptr);

                // 根据客户端请求，尝试从重传缓存里找分片并重发
                HandleNackRequest(client_address, now_ns, nack_header, nack_items);
            }
        }
    }

    running_ = false;
    CloseSocket();
}

// 注册或刷新一个 UDP 客户端。
// 参数 address：客户端的 IP 和端口。
// 参数 now_ns：当前时间，单位纳秒。
// 参数 report：客户端上报的接收统计，可以为空。
void UdpStreamingBackend::RegisterClient(
        const sockaddr_in &address,
        std::uint64_t now_ns,
        const common::net::UdpReceiverReport *report) {
    std::lock_guard<std::mutex> lock(clients_mutex_);

    // 注册新客户端前，先清理已经超时的旧客户端
    PruneStaleClientsLocked(now_ns);

    // 遍历当前已经记录的客户端列表
    for (std::size_t index = 0; index < clients_.size(); ++index) {
        // 判断当前 address 是否和 clients_[index] 是同一个客户端。
        if (SameEndpoint(clients_[index].address, address)) {
            clients_[index].last_seen_ns = now_ns;
            if (report != nullptr) {
                clients_[index].latest_report = *report;
                clients_[index].latest_report_timestamp_ns = now_ns;
                clients_[index].has_report = true;
                common::log::Logger::Info(FormatClientReport(clients_[index]));
            }
            return;
        }
    }

    // 如果循环结束都没有 return，说明 address 不在 clients_ 里，是一个新客户端。
    UdpClientEndpoint endpoint;

    endpoint.address = address;
    endpoint.last_seen_ns = now_ns;
    endpoint.latest_report_timestamp_ns = 0;
    endpoint.has_report = false;

    // 如果新客户端第一次发来的 keepalive 就带了 report，那也保存下来。
    if (report != nullptr) {
        endpoint.latest_report = *report;
        endpoint.latest_report_timestamp_ns = now_ns;
        endpoint.has_report = true;
    }

    // 把新客户端加入 clients_ 列表。
    clients_.push_back(endpoint);
    common::log::Logger::Info("udp client registered: " + FormatEndpoint(address));
    // 如果这个新客户端有接收报告，也打印出来
    if (endpoint.has_report) {
        common::log::Logger::Info(FormatClientReport(endpoint));
    }
}

// 删除超时的 UDP 客户端。
// 函数名带 Locked，表示调用者必须已经持有 clients_mutex_。
void UdpStreamingBackend::PruneStaleClientsLocked(std::uint64_t now_ns) {
    // 把毫秒转换成纳秒。
    //
    // config_.udp_client_timeout_ms 是毫秒。
    // now_ns 和 last_seen_ns 是纳秒。
    //
    // 1 ms = 1000 * 1000 ns
    const std::uint64_t timeout_ns = static_cast<std::uint64_t>(config_.udp_client_timeout_ms) * 1000ULL * 1000ULL;

    // erase + remove_if 是 C++ 里常见的“删除满足条件元素”的写法。
    clients_.erase(
            std::remove_if(
                    clients_.begin(),
                    clients_.end(),
                    [now_ns, timeout_ns](const UdpClientEndpoint &client) {
                        // 删除条件：
                        //
                        // 1. now_ns > client.last_seen_ns
                        //    当前时间确实晚于客户端最后出现时间。
                        //
                        // 2. now_ns - client.last_seen_ns > timeout_ns
                        //    客户端已经超过 timeout 没出现。
                        return now_ns > client.last_seen_ns && now_ns - client.last_seen_ns > timeout_ns;
                    }),

            // remove_if 会把不删除的元素移动到前面，返回新的逻辑结尾。
            // erase 再真正删除后面的无效元素。
            clients_.end());
}

// 获取当前在线客户端地址快照
std::vector<sockaddr_in> UdpStreamingBackend::SnapshotClients(std::uint64_t now_ns) {
    std::vector<sockaddr_in> snapshot;
    std::lock_guard<std::mutex> lock(clients_mutex_);

    // 先清理超时客户端，这样快照里不会包含已经离线的客户端
    PruneStaleClientsLocked(now_ns);

    // 提前分配空间，减少 vector 扩容次数
    snapshot.reserve(clients_.size());
    for (std::size_t index = 0; index < clients_.size(); ++index) {
        // 只复制 address。
        //
        // Broadcast 发送时只需要知道：我要往哪个 IP:端口发 UDP 包。
        snapshot.push_back(clients_[index].address);
    }
    return snapshot;
}

// 将一帧 EncodedFrame 拆分为多个 UDP 分片发送。
// 参数 frame：编码后的音视频帧，比如一帧 H.264。
// 参数 clients：当前在线客户端地址列表。
bool UdpStreamingBackend::SendFrameFragments(
        common::model::EncodedFramePtr frame,
        const std::vector<sockaddr_in> &clients) {
    if (!frame) {
        return false;
    }

    // 计算每个 UDP 包里的“协议头”总大小。
    //
    // 一个媒体 UDP 包结构是：
    // [MessageHeader][UdpFrameFragmentHeader][fragment payload]
    //
    // MessageHeader：
    //   项目通用消息头，说明这是哪类消息，比如音视频流、keepalive、NACK。
    //
    // UdpFrameFragmentHeader：
    //   UDP 分片头，说明这是第几帧、第几个分片、总共有几个分片。
    const std::size_t header_size = sizeof(common::net::MessageHeader) + sizeof(common::net::UdpFrameFragmentHeader);

    // 检查配置是否合理。
    //
    // udp_target_payload_size 是目标 UDP payload 大小。
    // udp_max_datagram_size 是单个 UDP 包允许的最大大小。
    //
    // 如果这两个值连 header_size 都放不下，那就根本没有空间放真正的视频数据。
    if (config_.udp_target_payload_size <= header_size || config_.udp_max_datagram_size <= header_size) {
        return false;
    }

    // 计算每个 fragment 最多能携带多少真正的媒体数据。
    //
    // 为什么取 min？
    //
    // 1. udp_target_payload_size：
    //    这是我们希望的目标大小。
    //
    // 2. udp_max_datagram_size - header_size：
    //    这是扣掉协议头后，实际最多还能放多少 payload。
    //
    // 两者取小，保证不会超过配置限制。
    const std::size_t max_fragment_payload_size = std::min(
            config_.udp_target_payload_size,
            config_.udp_max_datagram_size - header_size);

    // 计算这一帧需要拆成多少个 fragment。
    //
    // 如果 frame->payload 是空的，也至少发 1 个 fragment。
    // 这样接收端仍然能收到“这一帧存在，只是 payload 为空”的信息。
    //
    // 如果 payload 不为空，就做向上取整：
    //
    // fragment_count = ceil(payload_size / max_fragment_payload_size)
    //
    // 代码写法：
    // (payload_size + max_size - 1) / max_size
    const std::size_t fragment_count_size = frame->payload.empty()
            ? 1
            : (frame->payload.size() + max_fragment_payload_size - 1) / max_fragment_payload_size;

    // 协议里 fragment_count 用 uint16_t 表示。
    //
    // uint16_t 最大值是 65535，也就是 0xFFFF。
    // 如果一帧被拆成超过 65535 个 fragment，接收端协议字段就表示不了，所以只能丢弃这一帧。
    if (fragment_count_size > 0xFFFFu) {
        common::log::Logger::Warn("udp frame dropped because fragment count exceeds protocol limit");
        return false;
    }

    // 把 size_t 类型的 fragment_count_size 转成 uint16_t
    const std::uint16_t fragment_count = static_cast<std::uint16_t>(fragment_count_size);

    // 创建通用消息头
    common::net::MessageHeader header{};

    // 填充 magic
    common::net::FillMessageMagic(header.head_id);

    // 设置消息类型为音视频流
    header.message_type = static_cast<std::uint16_t>(common::net::MessageType::kAvStream);

    // 设置子类型为 frame->type
    header.sub_type = static_cast<std::uint16_t>(frame->type);

    // 记录当前准备发送的时间
    const std::uint64_t transport_send_timestamp_ns = common::time::MonotonicNowNs();

    // 准备一个缓存帧对象。
    //
    // 如果开启 NACK，服务端需要把已经发出去的 fragment 暂时缓存起来。
    // 客户端之后如果说“某个 fragment 丢了”，服务端才能重发。
    CachedUdpFrame cached_frame;

    // 记录这份缓存属于第几帧
    cached_frame.frame_sequence = frame->sequence;

    // 记录缓存创建时间
    cached_frame.cached_timestamp_ns = transport_send_timestamp_ns;

    // FEC 校验 payload。
    //
    // 如果开启 FEC，后面每生成一个普通数据分片，都会把它 XOR 到 fec_payload 里。
    // 最后 fec_payload 会作为一个额外的“校验分片”发出去
    std::vector<std::uint8_t> fec_payload;

    // send_datagram_ 是类成员，作为发送缓冲区复用。
    //
    // 如果当前大小不够，就扩容到 udp_max_datagram_size。
    //
    // 这样后面每个 fragment 都可以先拼到 send_datagram_ 里，
    // 再调用 sendto/sendmmsg 发出去。
    if (send_datagram_.size() < config_.udp_max_datagram_size) {
        send_datagram_.resize(config_.udp_max_datagram_size);
    }

    // 如果客户端数量大于 1，就启用批量发送。
    //
    // batch_send = true：
    //   使用 sendmmsg，一次系统调用发给多个客户端。
    //
    // batch_send = false：
    //   使用 sendto，只发给一个客户端。
    const bool batch_send = clients.size() > 1;

    // sendmmsg 需要 mmsghdr 数组。
    //
    // 每个 mmsghdr 描述一次要发送的消息：
    // - 发给谁
    // - 数据在哪里
    // - 数据长度是多少
    std::vector<struct mmsghdr> batch_messages;

    // iovec 描述一段内存。
    //
    // 这里每个 UDP 包只有一段连续内存 send_datagram_，所以每个客户端对应一个 iovec。
    std::vector<struct iovec> batch_iovecs;

    // 如果启用批量发送，就按照客户端数量分配数组大小。
    if (batch_send) {
        batch_messages.resize(clients.size());
        batch_iovecs.resize(clients.size());
    }

    // 遍历这一帧的所有 fragment
    for (std::uint16_t fragment_index = 0; fragment_index < fragment_count; ++fragment_index) {
        // 计算当前 fragment 在整帧 payload 里的起始偏移
        const std::size_t fragment_offset = static_cast<std::size_t>(fragment_index) * max_fragment_payload_size;

        // 计算当前 fragment 实际携带多少 payload 字节。
        const std::size_t fragment_payload_size = frame->payload.empty()
                ? 0
                : std::min(max_fragment_payload_size, frame->payload.size() - fragment_offset);

        // 创建当前 fragment 的 UDP 分片头。
        //
        // 这个头会告诉客户端：
        // - 这是第几帧
        // - 这是第几个 fragment
        // - 这一帧总共有几个 fragment
        // - 当前 fragment 对应原始 payload 的哪个 offset
        common::net::UdpFrameFragmentHeader fragment_header{};

        // 当前 fragment 属于哪一帧
        fragment_header.frame_sequence = frame->sequence;

        // 如果配置允许嵌入元数据，就写入采集时间戳
        fragment_header.capture_timestamp_ns = config_.embed_frame_metadata ? frame->capture_timestamp_ns : 0;

        // 如果配置允许嵌入元数据，就写入编码开始时间
        fragment_header.encode_start_timestamp_ns = config_.embed_frame_metadata ? frame->encode_start_timestamp_ns : 0;

        // 如果配置允许嵌入元数据，就写入编码结束时间
        fragment_header.encode_end_timestamp_ns = config_.embed_frame_metadata ? frame->encode_end_timestamp_ns : 0;

        // 如果配置允许嵌入元数据，就写入传输发送时间
        fragment_header.transport_send_timestamp_ns = config_.embed_frame_metadata ? transport_send_timestamp_ns : 0;

        // 记录整帧 payload 的大小
        fragment_header.frame_payload_size = static_cast<std::uint32_t>(frame->payload.size());

        // 当前 fragment 在整帧 payload 中的偏移
        fragment_header.fragment_offset = static_cast<std::uint32_t>(fragment_offset);

        // 当前 fragment 的编号
        fragment_header.fragment_index = fragment_index;

        // 当前帧总共有多少 fragment
        fragment_header.fragment_count = fragment_count;

        // 当前 fragment 的角色
        fragment_header.fragment_role = static_cast<std::uint16_t>(common::net::UdpFragmentRole::kData);

        // 设置 MessageHeader 里的 payload_length
        // 注意：这里的 payload_length 指的是 MessageHeader 后面的大小：[UdpFrameFragmentHeader][fragment payload]
        header.payload_length = static_cast<std::uint32_t>(sizeof(fragment_header) + fragment_payload_size);

        // 计算整个 UDP datagram 的大小。
        // 完整 UDP 包内容：[MessageHeader][UdpFrameFragmentHeader][fragment payload]
        const std::size_t datagram_size = sizeof(header) + sizeof(fragment_header) + fragment_payload_size;

        // 把 MessageHeader 拷贝到 send_datagram_ 开头。
        // send_datagram_ 现在布局：[MessageHeader][空][空]
        std::memcpy(send_datagram_.data(), &header, sizeof(header));

        // 把 UdpFrameFragmentHeader 拷贝到 MessageHeader 后面。
        // send_datagram_ 现在布局：[MessageHeader][UdpFrameFragmentHeader][空]
        std::memcpy(send_datagram_.data() + sizeof(header), &fragment_header, sizeof(fragment_header));

        // 如果当前 fragment 有真实 payload，就把对应那段 frame->payload 拷贝进去。
        if (fragment_payload_size > 0) {
            std::memcpy(
                    send_datagram_.data() + sizeof(header) + sizeof(fragment_header),
                    frame->payload.data() + fragment_offset,
                    fragment_payload_size);
        }

        // 如果开启 NACK，就把当前完整 UDP datagram 缓存起来。
        //
        // 为什么缓存完整 datagram，而不是只缓存 payload？
        // 因为重传时可以直接 sendto()，不需要重新构造 MessageHeader 和 UdpFrameFragmentHeader
        if (config_.udp_enable_nack) {
            // 创建一个缓存分片对象
            CachedUdpFragment cached_fragment;

            // 记录它是第几个 fragment
            cached_fragment.fragment_index = fragment_index;

            // 记录这个 UDP datagram 的实际大小
            cached_fragment.datagram_size = datagram_size;

            // 复制完整 UDP datagram
            cached_fragment.datagram.assign(send_datagram_.begin(), send_datagram_.begin() + static_cast<std::ptrdiff_t>(datagram_size));

            // 放入当前帧的缓存列表。
            //
            // 注意：
            // 此时只是先放到 cached_frame 里。等整帧所有 fragment 都发送完之后，
            // 才会调用 CacheFrameFragments(cached_frame) 把它放入全局 retransmit_cache_。
            cached_frame.fragments.push_back(cached_fragment);
        }

        // 如果开启 FEC，并且当前 fragment 有 payload，就把这个 payload XOR 到 fec_payload。
        if (config_.udp_enable_fec && fragment_payload_size > 0) {
            XorInto(&fec_payload, frame->payload.data() + fragment_offset, fragment_payload_size);
        }

        // 标记当前 fragment 是否至少成功发给了某个客户端。
        bool sent_fragment = false;

        // 如果客户端数量大于 1，使用 sendmmsg 批量发送。
        if (batch_send) {
            // 给每一个客户端准备一条发送描述
            for (std::size_t ci = 0; ci < clients.size(); ++ci) {
                // iov_base 指向要发送的数据。
                //
                // 这里所有客户端发送的内容相同，都是当前 send_datagram_。
                batch_iovecs[ci].iov_base = send_datagram_.data();
                batch_iovecs[ci].iov_len = datagram_size;

                // 清空当前 mmsghdr，避免残留字段影响发送
                std::memset(&batch_messages[ci], 0, sizeof(batch_messages[ci]));

                // 设置消息的数据段
                batch_messages[ci].msg_hdr.msg_iov = &batch_iovecs[ci];

                // 只有一个 iovec
                batch_messages[ci].msg_hdr.msg_iovlen = 1;
                batch_messages[ci].msg_hdr.msg_name = const_cast<sockaddr_in *>(&clients[ci]);
                batch_messages[ci].msg_hdr.msg_namelen = sizeof(clients[ci]);
            }

            // 批量发送。
            //
            // socket_fd_：UDP socket。
            // batch_messages.data()：发送消息数组。
            // batch_messages.size()：要发送多少个包。
            // MSG_NOSIGNAL：避免发送失败时触发 SIGPIPE。
            const int sent_count = sendmmsg(socket_fd_, batch_messages.data(),
                                             static_cast<unsigned int>(batch_messages.size()), MSG_NOSIGNAL);
            // sent_count > 0 表示至少成功发送给了一个客户端
            if (sent_count > 0) {
                // 当前 fragment 认为发送成功
                sent_fragment = true;

                // sent_count 是成功发送的 UDP 包数量
                sent_fragments_ += static_cast<std::uint64_t>(sent_count);

                // 没发成功的客户端数量计入 failed_fragments_
                failed_fragments_ += static_cast<std::uint64_t>(clients.size()) - static_cast<std::uint64_t>(sent_count);
            }

        // 如果只有一个客户端，使用 sendto
        } else if (!clients.empty()) {
            // 发送当前 UDP datagram 给 clients[0]
            const ssize_t sent = sendto(
                    socket_fd_,
                    send_datagram_.data(),
                    datagram_size,
                    MSG_NOSIGNAL,
                    reinterpret_cast<const sockaddr *>(&clients[0]),
                    sizeof(clients[0]));

            // UDP sendto 成功时，应该返回发送的字节数。
            // 如果等于 datagram_size，说明整个 datagram 被交给内核发送。
            if (sent == static_cast<ssize_t>(datagram_size)) {
                sent_fragment = true;
                ++sent_fragments_;
            } else {
                ++failed_fragments_;
            }
        }

        // 如果当前 fragment 没能发送给任何客户端，
        // 整帧发送失败，直接返回 false。
        if (!sent_fragment) {
            return false;
        }
    }

    // 发送 FEC 奇偶校验分片。
    //
    // 前面的 for 循环已经把每一个普通数据分片都 XOR 到 fec_payload 里了。
    //
    // 所以此时：
    //
    // fec_payload = fragment0 ^ fragment1 ^ fragment2 ^ ...
    //
    // 这个 fec_payload 会作为一个额外 UDP 包发出去。
    // 如果客户端只丢了其中 1 个普通数据分片，就可以用 fec_payload 和其他已收到分片恢复丢失分片
    if (config_.udp_enable_fec && fragment_count > 1 && !fec_payload.empty()) {
        // 创建 FEC 分片头。
        //
        // 它和普通数据分片一样，也使用 UdpFrameFragmentHeader。
        // 但是 fragment_role 不一样：
        // - 普通分片：kData
        // - FEC 分片：kXorParity
        common::net::UdpFrameFragmentHeader parity_header{};
        parity_header.frame_sequence = frame->sequence;
        parity_header.capture_timestamp_ns = config_.embed_frame_metadata ? frame->capture_timestamp_ns : 0;
        parity_header.encode_start_timestamp_ns = config_.embed_frame_metadata ? frame->encode_start_timestamp_ns : 0;
        parity_header.encode_end_timestamp_ns = config_.embed_frame_metadata ? frame->encode_end_timestamp_ns : 0;
        parity_header.transport_send_timestamp_ns = config_.embed_frame_metadata ? transport_send_timestamp_ns : 0;
        parity_header.frame_payload_size = static_cast<std::uint32_t>(frame->payload.size());

        // FEC 分片不是原始帧中的某一段连续数据，所以 offset 写 0。
        parity_header.fragment_offset = 0;

        // FEC 在这里不是“第几个普通数据分片”，所以 fragment_index 写 0。
        parity_header.fragment_index = 0;
        parity_header.fragment_count = fragment_count;

        // 标记这是 XOR FEC 校验分片，不是普通数据分片。
        parity_header.fragment_role = static_cast<std::uint16_t>(common::net::UdpFragmentRole::kXorParity);

        // 设置通用消息头里的 payload_length。
        // FEC UDP 包结构：[MessageHeader][UdpFrameFragmentHeader][fec_payload]
        header.payload_length = static_cast<std::uint32_t>(sizeof(parity_header) + fec_payload.size());

        // 计算 FEC UDP 包总大小
        const std::size_t parity_datagram_size = sizeof(header) + sizeof(parity_header) + fec_payload.size();

        // 把 MessageHeader 拷贝进发送缓冲区
        std::memcpy(send_datagram_.data(), &header, sizeof(header));

        // 把 FEC 分片头拷贝到 MessageHeader 后面
        std::memcpy(send_datagram_.data() + sizeof(header), &parity_header, sizeof(parity_header));

        // 标记 FEC 分片是否发送成功
        std::memcpy(
                send_datagram_.data() + sizeof(header) + sizeof(parity_header),
                fec_payload.data(),
                fec_payload.size());

        // 标记 FEC 分片是否发送成功
        bool sent_parity = false;

        // 如果有多个客户端，使用 sendmmsg 批量发送
        if (batch_send) {
            // 给每个客户端构造一个发送描述
            for (std::size_t ci = 0; ci < clients.size(); ++ci) {
                // 设置发送数据起始地址
                batch_iovecs[ci].iov_base = send_datagram_.data();

                // 设置发送数据长度
                batch_iovecs[ci].iov_len = parity_datagram_size;

                // 清空 mmsghdr，防止上一次循环残留字段
                std::memset(&batch_messages[ci], 0, sizeof(batch_messages[ci]));

                batch_messages[ci].msg_hdr.msg_iov = &batch_iovecs[ci];
                batch_messages[ci].msg_hdr.msg_iovlen = 1;
                batch_messages[ci].msg_hdr.msg_name = const_cast<sockaddr_in *>(&clients[ci]);
                batch_messages[ci].msg_hdr.msg_namelen = sizeof(clients[ci]);
            }

            // 批量发送 FEC 分片给多个客户端
            const int sent_count = sendmmsg(socket_fd_, batch_messages.data(),
                                             static_cast<unsigned int>(batch_messages.size()), MSG_NOSIGNAL);
            // sent_count > 0 表示至少发给了一个客户端
            if (sent_count > 0) {
                sent_parity = true;
                sent_fragments_ += static_cast<std::uint64_t>(sent_count);
                fec_fragments_sent_ += static_cast<std::uint64_t>(sent_count);
                failed_fragments_ += static_cast<std::uint64_t>(clients.size()) - static_cast<std::uint64_t>(sent_count);
            }

        // 如果只有一个客户端，使用 sendto
        } else if (!clients.empty()) {
            const ssize_t sent = sendto(
                    socket_fd_,
                    send_datagram_.data(),
                    parity_datagram_size,
                    MSG_NOSIGNAL,
                    reinterpret_cast<const sockaddr *>(&clients[0]),
                    sizeof(clients[0]));

            // 判断是否完整发送
            if (sent == static_cast<ssize_t>(parity_datagram_size)) {
                sent_parity = true;
                ++sent_fragments_;
                ++fec_fragments_sent_;
            } else {
                ++failed_fragments_;
            }
        }

        // 如果 FEC 分片没能发送给任何客户端，认为这一帧发送失败
        if (!sent_parity) {
            return false;
        }
    }

    // 如果开启 NACK，并且前面确实缓存了普通数据分片，就把当前帧的所有普通分片正式放入重传缓存。
    //
    // 注意：
    // FEC 分片没有放入 cached_frame.fragments。
    // 当前代码主要缓存普通数据分片用于 NACK 重传。
    if (config_.udp_enable_nack && !cached_frame.fragments.empty()) {
        // 把这一帧的所有 fragment 放入 retransmit_cache_。
        //
        // 之后客户端发 NACK：
        // “第 N 帧第 K 个分片丢了”
        //
        // 服务端就能从 retransmit_cache_ 里找到并重发。
        CacheFrameFragments(cached_frame);
    }

    // 整帧所有普通分片发送成功，FEC 如果开启也发送成功，NACK 缓存也处理完成。
    return true;
}

// 把某一帧的所有 UDP 分片放入重传缓存。
// 参数 frame：已经发送过、准备缓存起来的 UDP 帧。
void UdpStreamingBackend::CacheFrameFragments(const CachedUdpFrame &frame) {
    std::lock_guard<std::mutex> lock(retransmit_cache_mutex_);
    // 先清理已经过期的缓存帧。这样 retransmit_cache_ 不会无限变大。
    PruneRetransmitCacheLocked(common::time::MonotonicNowNs());

    // 把当前帧加入重传缓存队列尾部。
    // retransmit_cache_ 可以理解成：最近发送过的若干帧。
    retransmit_cache_.push_back(frame);

    // 如果缓存帧数超过配置上限，就从队头删除最老的帧。
    //
    // 举例：
    // udp_retransmit_cache_frames = 60
    //
    // 最多缓存最近 60 帧。
    // 如果加入第 61 帧，就删除最老的第 1 帧。
    while (retransmit_cache_.size() > config_.udp_retransmit_cache_frames) {
        retransmit_cache_.pop_front();
    }
}

// 清理过期的重传缓存。
// 函数名带 Locked，表示调用者必须已经持有 retransmit_cache_mutex_
void UdpStreamingBackend::PruneRetransmitCacheLocked(std::uint64_t now_ns) {
    // 把配置的毫秒时间转换成纳秒。
    //
    // config_.udp_retransmit_cache_max_age_ms 是毫秒。
    // now_ns 和 cached_timestamp_ns 是纳秒。
    //
    // 1 ms = 1000 * 1000 ns
    const std::uint64_t max_age_ns =
            static_cast<std::uint64_t>(config_.udp_retransmit_cache_max_age_ms) * 1000ULL * 1000ULL;

    // retransmit_cache_ 是按时间顺序 push_back 的。
    // 最老的帧在 front，所以只需要不断检查 front 是否过期。
    while (!retransmit_cache_.empty()) {
        // 取出最老的缓存帧
        const CachedUdpFrame &frame = retransmit_cache_.front();

        // 判断这帧缓存是否超过最大保留时间。
        //
        // now_ns >= frame.cached_timestamp_ns：
        // 防止时间异常倒退。
        //
        // now_ns - frame.cached_timestamp_ns > max_age_ns：
        // 说明这帧缓存已经太老了。
        if (now_ns >= frame.cached_timestamp_ns && now_ns - frame.cached_timestamp_ns > max_age_ns) {
            retransmit_cache_.pop_front();
            continue;
        }

        // 如果 front 都没过期，后面的帧更晚加入，
        // 一定也没过期，所以可以停止。
        break;
    }
}

// 处理客户端 NACK 重传请求。
// 客户端告诉服务端：“这些分片我没收到，请重发。”
void UdpStreamingBackend::HandleNackRequest(
        const sockaddr_in &address,
        std::uint64_t now_ns,
        const common::net::UdpNackHeader & /* nack_header */,
        const std::vector<common::net::UdpNackItem> &nack_items) {

    // 如果没有开启 NACK，直接返回。
    // 如果 nack_items 为空，说明客户端没有真正请求任何分片。
    // 如果 socket_fd_ < 0，说明 UDP socket 无效，无法重发。
    if (!config_.udp_enable_nack || nack_items.empty() || socket_fd_ < 0) {
        return;
    }

    {
        std::lock_guard<std::mutex> lock(retransmit_cache_mutex_);
        // 先清理过期缓存，否则可能会在过期数据里白找。
        PruneRetransmitCacheLocked(now_ns);
    }

    // NACK 请求次数 +1
    ++nack_requests_received_;

    // 客户端请求重传的 fragment 总数增加
    nack_fragments_requested_ += nack_items.size();

    // 再次加锁，准备真正查找并重发
    std::lock_guard<std::mutex> lock(retransmit_cache_mutex_);

    // 限制本次最多处理多少个 fragment。
    //
    // 为什么要限制？
    // 如果客户端一次请求几千个分片，服务端可能被重传拖垮。
    //
    // udp_retransmit_max_fragments_per_request 就是单次 NACK 的重传上限。
    const std::size_t allowed_fragments =
            std::min<std::size_t>(nack_items.size(), config_.udp_retransmit_max_fragments_per_request);

    // 如果客户端请求数量超过允许数量，超出的部分计入 throttled。
    if (nack_items.size() > allowed_fragments) {
        retransmit_fragments_throttled_ += nack_items.size() - allowed_fragments;
    }

    // 遍历允许处理的 NACK item
    for (std::size_t item_index = 0; item_index < allowed_fragments; ++item_index) {
        // 当前请求重传的分片
        const common::net::UdpNackItem &item = nack_items[item_index];

        // 标记是否在缓存中找到了这个分片
        bool found_fragment = false;

        // 遍历重传缓存里的每一帧
        for (std::size_t frame_index = 0; frame_index < retransmit_cache_.size() && !found_fragment; ++frame_index) {
            // 当前缓存帧。
            const CachedUdpFrame &cached_frame = retransmit_cache_[frame_index];

            // 如果帧序号不匹配，说明不是客户端要的那一帧
            if (cached_frame.frame_sequence != item.frame_sequence) {
                continue;
            }

            // 找到了对应帧，再遍历这一帧里的所有缓存分片
            for (std::size_t fragment_index = 0; fragment_index < cached_frame.fragments.size(); ++fragment_index) {
                // 当前缓存分片
                const CachedUdpFragment &cached_fragment = cached_frame.fragments[fragment_index];

                // 如果 fragment_index 不匹配，说明不是客户端要的那个分片。
                if (cached_fragment.fragment_index != item.fragment_index) {
                    continue;
                }

                // 找到了客户端请求的分片。
                //
                // cached_fragment.datagram 是之前缓存下来的完整 UDP 包：
                // [MessageHeader][UdpFrameFragmentHeader][payload fragment]
                //
                // 所以重传时不需要重新组包，直接 sendto。
                const ssize_t sent = sendto(
                        socket_fd_,
                        cached_fragment.datagram.data(),
                        cached_fragment.datagram_size,
                        MSG_NOSIGNAL,
                        reinterpret_cast<const sockaddr *>(&address),
                        sizeof(address));

                // 如果完整发送成功，重传成功计数 +1
                if (sent == static_cast<ssize_t>(cached_fragment.datagram_size)) {
                    ++retransmitted_fragments_sent_;
                }

                // 不管 sendto 是否成功，只要缓存里找到了这个 fragment，
                // found_fragment 就设为 true。
                found_fragment = true;
                break;
            }
        }

        // 如果遍历完缓存都没有找到这个 fragment，
        // 说明缓存里已经没有了。
        //
        // 常见原因：
        // - 客户端请求太晚，缓存过期了
        // - 缓存帧数上限太小，旧帧被淘汰了
        // - 请求里的 frame_sequence/fragment_index 本身不对
        if (!found_fragment) {
            ++retransmit_fragment_misses_;
        }
    }
}

// 把 sockaddr_in 格式的 IPv4 地址转换成人能看懂的字符串。
// 例如：192.168.1.23:50100
std::string UdpStreamingBackend::FormatEndpoint(const sockaddr_in &address) const {
    // 准备一个字符数组，用来保存 IP 字符串
    char ip_buffer[INET_ADDRSTRLEN] = {0};

    // 把二进制 IPv4 地址转换成字符串形式
    inet_ntop(AF_INET, &address.sin_addr, ip_buffer, sizeof(ip_buffer));
    return std::string(ip_buffer) + ":" + std::to_string(ntohs(address.sin_port));
}

// 把客户端上报的接收情况格式化成日志字符串
std::string UdpStreamingBackend::FormatClientReport(const UdpClientEndpoint &client) const {
     // 取出客户端最近一次上报的接收报告
    const common::net::UdpReceiverReport &report = client.latest_report;

    // 计算“尝试接收过的 fragment 总数”。
    //
    // fragments_received：
    //   客户端实际收到的 fragment 数量。
    //
    // timed_out_fragments：
    //   客户端等了一段时间仍然没等到的 fragment 数量。
    //
    // 二者相加可以粗略理解为：
    // 客户端认为应该处理过的 fragment 总量。
    const std::uint64_t total_fragment_attempts = report.fragments_received + report.timed_out_fragments;

    // 计算 fragment 丢失率。
    //
    // 如果 total_fragment_attempts 是 0，
    // 说明还没有统计样本，丢失率按 0.0 处理，避免除以 0。
    //
    // 否则：
    // timed_out_fragments / total_fragment_attempts * 100%
    const double fragment_loss_percent = total_fragment_attempts == 0
            ? 0.0
            : static_cast<double>(report.timed_out_fragments) * 100.0 / static_cast<double>(total_fragment_attempts);

    // 拼接一条日志字符串。
    return "udp client report"
            " endpoint=" + FormatEndpoint(client.address) +
            " completed_frames=" + std::to_string(report.completed_frames) +
            " reordered_frames=" + std::to_string(report.reordered_frames) +
            " fragments=" + std::to_string(report.fragments_received) +
            " duplicate_fragments=" + std::to_string(report.duplicate_fragments) +
            " timed_out_fragments=" + std::to_string(report.timed_out_fragments) +
            " timed_out_frames=" + std::to_string(report.timed_out_frames) +
            " invalid_datagrams=" + std::to_string(report.invalid_datagrams) +
            " fragment_loss=" + std::to_string(fragment_loss_percent) + "%" +
            " jitter_avg=" + std::to_string(report.jitter_avg_ms) + "ms" +
            " jitter_max=" + std::to_string(report.jitter_max_ms) + "ms";
}

// 判断两个 UDP endpoint 是否是同一个客户端。
bool UdpStreamingBackend::SameEndpoint(const sockaddr_in &lhs, const sockaddr_in &rhs) const {

    // UDP 客户端身份基本由：
    // IP 地址 + 端口号
    // 决定。
    //
    // lhs.sin_port == rhs.sin_port：
    //   端口相同。
    //
    // lhs.sin_addr.s_addr == rhs.sin_addr.s_addr：
    //   IPv4 地址相同。
    //
    // 二者都相同，就认为是同一个客户端。
    return lhs.sin_port == rhs.sin_port && lhs.sin_addr.s_addr == rhs.sin_addr.s_addr;
}

}  // namespace udp
}  // namespace transport
}  // namespace modules
}  // namespace sserver
