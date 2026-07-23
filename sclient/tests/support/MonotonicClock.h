#ifndef SCLIENT_TESTS_SUPPORT_MONOTONICCLOCK_H
#define SCLIENT_TESTS_SUPPORT_MONOTONICCLOCK_H

#include <cstdint>
#include <ctime>

namespace sclient {
namespace tests {
namespace support {

/** 获取单调时钟当前时间（纳秒），用于测试中的时间测量 */
inline std::uint64_t MonotonicNowNs() {
    timespec timestamp{};
    clock_gettime(CLOCK_MONOTONIC, &timestamp);
    return static_cast<std::uint64_t>(timestamp.tv_sec) * 1000000000ULL +
           static_cast<std::uint64_t>(timestamp.tv_nsec);
}

}  // namespace support
}  // namespace tests
}  // namespace sclient

#endif  // SCLIENT_TESTS_SUPPORT_MONOTONICCLOCK_H
