#include "modules/network/StreamClientInternal.h"

namespace sclient {

namespace {

constexpr std::uint32_t kMaxPayloadLength = 50 * 1024 * 1024;
constexpr int kMaxConsecutiveTimeouts = 10;

}  // namespace

using network_internal::BuildSocketError;
using network_internal::MonotonicNowNs;

// 从 TCP socket 中读取指定长度的数据。
// buffer：数据写入位置。
// length：期望读取的字节数。
// error_message：失败时写入错误原因。
bool StreamClient::ReceiveAll(char *buffer, std::size_t length, std::string *error_message) {
    // 当前已经成功接收到的字节数
    std::size_t received = 0;

    // 连续接收超时次数
    int consecutive_timeouts = 0;

    // 只要还没读满 length 字节，就继续 recv
    while (received < length) {
        const ssize_t result = recv(socket_fd_, buffer + received, length - received, 0);

        if (result < 0) {
            // EINTR 表示系统调用被信号打断。
            if (errno == EINTR) {
                continue;
            }

            // EAGAIN / EWOULDBLOCK 通常表示当前没有数据可读，
            // 或者因为设置了 SO_RCVTIMEO，接收超时了。
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                if (++consecutive_timeouts >= kMaxConsecutiveTimeouts) {
                    if (error_message != nullptr) {
                        *error_message = "receive timed out";
                    }
                    return false;
                }
                continue;
            }
            if (error_message != nullptr) {
                *error_message = BuildSocketError("failed to receive data");
            }
            return false;
        }

        // result == 0 表示对端关闭了 TCP 连接。
        // 也就是服务端主动断开或退出了。
        if (result == 0) {
            if (error_message != nullptr) {
                *error_message = "server closed the connection";
            }
            return false;
        }

        // result > 0 表示本次成功收到了 result 个字节。
        // 把它累加到 received。
        received += static_cast<std::size_t>(result);

        // 既然成功收到数据，就说明网络不是连续超时状态。
        // 清零连续超时计数。
        consecutive_timeouts = 0;
    }
    return true;
}

// TCP 模式下接收一整帧视频数据。
// 输出到 frame。
// 成功时 frame 里会包含：header、metadata、H.264 payload、receive_timestamp。
bool StreamClient::ReceiveTcpFrame(ReceivedFrame *frame, std::string *error_message) {
    // 第一步：读取固定长度的 MessageHeader。
    //
    // TCP 没有消息边界，所以必须先读 header，
    // 再根据 header.payload_length 知道后面还有多少数据。
    if (!ReceiveAll(reinterpret_cast<char *>(&frame->header), sizeof(frame->header), error_message)) {
        return false;
    }

    // 第二步：校验 MessageHeader 的 magic
    if (!protocol::HasValidMessageMagic(frame->header)) {
        if (error_message != nullptr) {
            *error_message = "received invalid message magic";
        }
        return false;
    }

    // 第三步：从 header 里取 payload_length
    std::uint32_t payload_length = frame->header.payload_length;

    // 防御性检查：payload 不能超过 50MB。
    // 如果超过，可能是协议错乱或异常数据。
    if (payload_length > kMaxPayloadLength) {
        if (error_message != nullptr) {
            *error_message = "payload length exceeds maximum";
        }
        return false;
    }

    // 默认认为发送端 metadata 不可用
    frame->sender_metadata_available = false;

    // 如果客户端配置里期望服务端发送 metadata，
    // 就需要先从 TCP 流里读取 FrameDiagnosticMetadata
    if (config_.expect_metadata) {
        if (payload_length < sizeof(frame->metadata)) {
            if (error_message != nullptr) {
                *error_message = "payload is smaller than metadata";
            }
            return false;
        }

        // 从 TCP 流中读取 metadata。
        //
        // 现在 TCP 流位置在：
        // [MessageHeader] 已读完
        // [FrameDiagnosticMetadata] 下一段要读
        //
        // 所以这里读 sizeof(frame->metadata) 字节。
        if (!ReceiveAll(reinterpret_cast<char *>(&frame->metadata), sizeof(frame->metadata), error_message)) {
            return false;
        }
        payload_length -= sizeof(frame->metadata);

        // 检查 metadata 里是否真的有有效的发送端延迟时间戳
        frame->sender_metadata_available = network_internal::HasSenderLatencyMetadata(frame->metadata);

    // 如果客户端不期望 metadata
    } else {
        // 把 metadata 清零，避免残留旧值被误用
        std::memset(&frame->metadata, 0, sizeof(frame->metadata));
    }

    // 根据最终的 payload_length 分配 H.264 payload 缓冲区
    frame->payload.resize(payload_length);

    // 如果 payload 非空，就继续从 TCP 流中读取完整 H.264 数据
    if (!frame->payload.empty() &&
        !ReceiveAll(reinterpret_cast<char *>(frame->payload.data()), frame->payload.size(), error_message)) {
        return false;
    }

    // 所有内容都读完后，记录客户端完整收到这一帧的时间
    frame->receive_timestamp_ns = MonotonicNowNs();
    return true;
}

}  // namespace sclient
