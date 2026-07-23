#ifndef SCLIENT_MODULES_NETWORK_STREAMCLIENTINTERNAL_H
#define SCLIENT_MODULES_NETWORK_STREAMCLIENTINTERNAL_H

#include <errno.h>
#include <time.h>

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

#include "modules/network/StreamClient.h"

namespace sclient {
namespace network_internal {

/** UDP 帧组装超时时间（250ms） */
constexpr std::uint64_t kUdpAssemblyTimeoutNs = 250ULL * 1000ULL * 1000ULL;
/** 最近完成帧序列的窗口大小（用于去重） */
constexpr std::size_t kRecentCompletedUdpSequenceWindow = 1024;

/** 构建包含 errno 描述的 socket 错误信息 */
inline std::string BuildSocketError(const std::string &message) {
    return message + ": " + std::strerror(errno);
}

/** 获取单调时钟当前时间（纳秒） */
inline std::uint64_t MonotonicNowNs() {
    timespec timestamp{};
    clock_gettime(CLOCK_MONOTONIC, &timestamp);
    return static_cast<std::uint64_t>(timestamp.tv_sec) * 1000000000ULL +
           static_cast<std::uint64_t>(timestamp.tv_nsec);
}

/** 检查帧元数据是否包含有效的延迟测量时间戳 */
inline bool HasSenderLatencyMetadata(const protocol::FrameDiagnosticMetadata &metadata) {
    return metadata.capture_timestamp_ns != 0 && metadata.transport_send_timestamp_ns != 0;
}

/**
 * 将 source 数据 XOR 到 target 中
 *
 * 用于 FEC 前向纠错：data1 XOR data2 = parity，丢失任意一个可通过另外两个恢复
 *
 * @param target 目标缓冲区（会被修改）
 * @param source 源数据
 * @param length 数据长度
 */
inline void XorInto(std::vector<std::uint8_t> *target, const std::uint8_t *source, std::size_t length) {
    if (target == nullptr || source == nullptr || length == 0) {
        return;
    }

    if (target->size() < length) {
        target->resize(length, 0);
    }
    for (std::size_t index = 0; index < length; ++index) {
        (*target)[index] ^= source[index];
    }
}

}  // namespace network_internal
}  // namespace sclient

#endif  // SCLIENT_MODULES_NETWORK_STREAMCLIENTINTERNAL_H
