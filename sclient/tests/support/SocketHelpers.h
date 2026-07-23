#ifndef SCLIENT_TESTS_SUPPORT_SOCKETHELPERS_H
#define SCLIENT_TESTS_SUPPORT_SOCKETHELPERS_H

#include <sys/socket.h>

#include <cstddef>

namespace sclient {
namespace tests {
namespace support {

/**
 * 通过 socket 发送全部数据
 *
 * 处理部分发送的情况，确保所有数据都被发送
 *
 * @param socket_fd socket 文件描述符
 * @param data 数据指针
 * @param size 数据大小
 * @return 全部发送成功返回 true
 */
inline bool SendAll(int socket_fd, const void *data, std::size_t size) {
    const char *cursor = reinterpret_cast<const char *>(data);
    std::size_t remaining = size;
    while (remaining > 0) {
        const ssize_t sent = send(socket_fd, cursor, remaining, MSG_NOSIGNAL);
        if (sent <= 0) {
            return false;
        }
        cursor += sent;
        remaining -= static_cast<std::size_t>(sent);
    }
    return true;
}

}  // namespace support
}  // namespace tests
}  // namespace sclient

#endif  // SCLIENT_TESTS_SUPPORT_SOCKETHELPERS_H
