#include "modules/transport/tcp/TcpStreamingBackend.h"

#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include <algorithm>
#include <chrono>

#include "common/log/Logger.h"

namespace sserver {
namespace modules {
namespace transport {
namespace tcp {

TcpStreamingBackend::TcpStreamingBackend(const std::shared_ptr<common::metrics::LatencyRecorder> &send_latency_recorder)
        : backend_name_("tcp"),
          latency_log_interval_frames_(120),
          listen_socket_fd_(-1),
          bound_port_(0),
          running_(false),
          send_latency_recorder_(send_latency_recorder),
          state_(core::ModuleState::kCreated) {
}

TcpStreamingBackend::~TcpStreamingBackend() {
    shutdown();
}

// 初始化 TCP 后端
bool TcpStreamingBackend::initialize(const core::ApplicationContext &context) {
    // 保存 transport 配置
    config_ = context.config.transport;

    // 保存延迟日志输出间隔
    latency_log_interval_frames_ = context.config.runtime.latency_log_interval_frames;

    // 初始化完成后，将状态置为 Initialized
    state_ = core::ModuleState::kInitialized;
    return true;
}

// 启动 TCP 后端
bool TcpStreamingBackend::start() {
    // 如果配置中关闭了 transport，则不真正启动 socket
    if (!config_.enabled) {
        state_ = core::ModuleState::kRunning;
        return true;
    }

    // 创建、绑定并监听 TCP socket
    if (!OpenListenSocket()) {
        state_ = core::ModuleState::kFailed;
        return false;
    }

    // 设置运行标志
    running_ = true;

    // 启动 accept 线程
    accept_thread_ = std::thread(&TcpStreamingBackend::AcceptLoop, this);

    // 启动成功，状态切换为 Running
    state_ = core::ModuleState::kRunning;
    return true;
}

// 停止 TCP 后端
void TcpStreamingBackend::stop() {
    // 如果当前不是 Running 状态，说明没有正在运行的服务需要停止
    if (state_.load() != core::ModuleState::kRunning) {
        return;
    }

    // 通知 accept 线程退出循环
    running_ = false;
    CloseListenSocket();

    // 等待 accept 线程结束
    if (accept_thread_.joinable()) {
        accept_thread_.join();
    }

    // 将 clients_ 中的客户端会话移动到局部变量 clients_copy
    //
    // 这样做的好处是：
    //      1. 持锁时间很短；
    //      2. 后续 Stop() 客户端时不再持有 clients_mutex_；
    //      3. 避免 Stop() 内部如果触发其他锁操作导致死锁风险。
    std::vector<std::shared_ptr<TcpClientSession>> clients_copy;
    {
        std::lock_guard<std::mutex> lock(clients_mutex_);
        clients_copy.swap(clients_);
    }

    // 停止所有客户端会话
    for (std::size_t index = 0; index < clients_copy.size(); ++index) {
        clients_copy[index]->Stop();
    }

    // TCP 后端停止完成
    state_ = core::ModuleState::kStopped;
}

// 关闭 TCP 后端并释放资源
void TcpStreamingBackend::shutdown() {
    stop();
    state_ = core::ModuleState::kShutdown;
}

// 返回当前 TCP 后端状态
core::ModuleState TcpStreamingBackend::state() const {
    return state_.load();
}

// 广播一帧编码后的数据给所有 TCP 客户端。
//
// frame 通常来自编码模块，包含 H.264 payload、帧序号、时间戳等信息。
// Broadcast() 不直接发送 socket，而是将 frame 投递给每个 TcpClientSession。
// 每个 session 再负责自己的发送队列和发送逻辑。
void TcpStreamingBackend::Broadcast(common::model::EncodedFramePtr frame) {
    // 如果 transport 未启用，或者 frame 为空，则直接返回
    if (!config_.enabled || !frame) {
        return;
    }

    // 复制当前客户端列表快照
    std::vector<std::shared_ptr<TcpClientSession>> snapshot;
    {
        std::lock_guard<std::mutex> lock(clients_mutex_);
        snapshot = clients_;
    }

    // 遍历客户端快照，将这一帧加入每个在线客户端的发送队列
    for (std::size_t index = 0; index < snapshot.size(); ++index) {
        if (snapshot[index]->IsRunning()) {
            snapshot[index]->EnqueueFrame(frame);
        }
    }

    // 清理已经断开的客户端，避免 clients_ 中长期保留无效会话
    PruneClosedClients();
}

// 返回实际绑定端口
int TcpStreamingBackend::bound_port() const {
    return bound_port_;
}

// 返回当前后端类型
TransportBackend TcpStreamingBackend::backend() const {
    return TransportBackend::kTcp;
}

// 返回当前后端名称
const std::string &TcpStreamingBackend::backend_name() const {
    return backend_name_;
}

// 创建、绑定并监听 TCP socket
bool TcpStreamingBackend::OpenListenSocket() {
    // 创建 TCP socket
    listen_socket_fd_ = socket(AF_INET, SOCK_STREAM, 0);
    if (listen_socket_fd_ < 0) {
        common::log::Logger::Error("failed to create tcp listen socket");
        return false;
    }

    // 设置地址复用，SO_REUSEADDR 可以减少服务重启时 bind 失败的概率
    int reuse = 1;
    setsockopt(listen_socket_fd_, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));

    // 将监听 socket 设置为非阻塞模式
    // 非阻塞后，accept() 在没有客户端连接时不会一直卡住，而是返回 -1，并设置 errno 为 EAGAIN 或 EWOULDBLOCK
    const int current_flags = fcntl(listen_socket_fd_, F_GETFL, 0);
    if (current_flags >= 0) {
        fcntl(listen_socket_fd_, F_SETFL, current_flags | O_NONBLOCK);
    }

    // 构造 IPv4 地址结构
    sockaddr_in address{};

    // IPv4 地址族
    address.sin_family = AF_INET;

    // 设置监听端口
    address.sin_port = htons(static_cast<std::uint16_t>(config_.listen_port));

    // 设置绑定地址
    address.sin_addr.s_addr = inet_addr(config_.bind_address.c_str());

    // 绑定 socket 到指定 IP 和端口
    if (bind(listen_socket_fd_, reinterpret_cast<sockaddr *>(&address), sizeof(address)) < 0) {
        common::log::Logger::Error("failed to bind tcp transport server");
        CloseListenSocket();
        return false;
    }

    // 开始监听客户端连接。
    //
    // 第二个参数 16 是监听队列长度 backlog，表示内核可以暂存的未 accept 连接数量。
    if (listen(listen_socket_fd_, 16) < 0) {
        common::log::Logger::Error("failed to listen on tcp transport socket");
        CloseListenSocket();
        return false;
    }

    // 如果配置端口为 0，表示让操作系统自动分配一个空闲端口
    // 此时需要通过 getsockname() 查询最终绑定到哪个端口。
    if (config_.listen_port == 0) {
        sockaddr_in bound_address{};
        socklen_t bound_length = sizeof(bound_address);
        if (getsockname(listen_socket_fd_, reinterpret_cast<sockaddr *>(&bound_address), &bound_length) == 0) {
            bound_port_ = ntohs(bound_address.sin_port);
            common::log::Logger::Info("tcp transport bound to ephemeral port " + std::to_string(bound_port_));
        }
    } else {
        // 固定端口模式下，实际端口就是配置端口
        bound_port_ = config_.listen_port;
    }

    return true;
}

// 关闭监听 socket
void TcpStreamingBackend::CloseListenSocket() {
    if (listen_socket_fd_ >= 0) {
        close(listen_socket_fd_);
        listen_socket_fd_ = -1;
    }
}

// accept 线程主循环
void TcpStreamingBackend::AcceptLoop() {
    while (running_.load()) {
        sockaddr_in client_address{};
        socklen_t address_length = sizeof(client_address);

        // 接收一个客户端连接。
        // 因为 listen_socket_fd_ 被设置为非阻塞，所以没有客户端连接时，accept() 不会一直阻塞
        const int client_fd = accept(listen_socket_fd_, reinterpret_cast<sockaddr *>(&client_address), &address_length);

        if (client_fd < 0) {
            // 非阻塞 accept 下，没有新连接是正常情况。
            //
            // EAGAIN / EWOULDBLOCK：
            //      当前没有可 accept 的连接。
            //
            // EINTR：
            //      accept 被信号中断，可以继续重试。
            if (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR) {
                std::this_thread::sleep_for(std::chrono::milliseconds(config_.accept_loop_interval_ms));
                continue;
            }

            // 其他错误通常说明监听 socket 出现异常
            common::log::Logger::Warn("accept failed on tcp transport socket");
            break;
        }

        // 为新客户端创建会话对象
        std::shared_ptr<TcpClientSession> session(
                new TcpClientSession(client_fd, config_, send_latency_recorder_, latency_log_interval_frames_));

        // 启动客户端会话
        if (!session->Start()) {
            session->Stop();
            continue;
        }

        common::log::Logger::Info("tcp client connected: " + session->remote_endpoint());

        // 将客户端会话加入 clients_
        {
            std::lock_guard<std::mutex> lock(clients_mutex_);
            clients_.push_back(session);
        }

        // 顺手清理已经断开的旧客户端
        PruneClosedClients();
    }
}

// 清理已经关闭的客户端会话
//
// clients_ 中保存的是 shared_ptr<TcpClientSession>。
// 如果某个客户端已经断开或者 session 停止运行，就需要从 clients_ 中移除，避免后续继续投递帧。
void TcpStreamingBackend::PruneClosedClients() {
    std::lock_guard<std::mutex> lock(clients_mutex_);
    clients_.erase(
            // remove_if 它会遍历 clients_，把不需要删除的元素移动到 vector 前面
            std::remove_if(
                    clients_.begin(),
                    clients_.end(),
                    [](const std::shared_ptr<TcpClientSession> &client) {
                        return client == nullptr || !client->IsRunning();
                    }),
            clients_.end());
}

}  // namespace tcp
}  // namespace transport
}  // namespace modules
}  // namespace sserver
