#ifndef SSERVER_COMMON_TIME_MONOTONICCLOCK_H
#define SSERVER_COMMON_TIME_MONOTONICCLOCK_H

#include <chrono>
#include <cstdint>

namespace sserver {
namespace common {
namespace time {

// 返回单调时钟当前值，单位纳秒。
//
// steady_clock 不受系统时间修改和 NTP 校时影响，适合计算耗时与延迟。
// 它不是现实日期时间，只能与同一时钟域的时间戳相减。
inline std::uint64_t MonotonicNowNs() {
    return static_cast<std::uint64_t>(
            std::chrono::duration_cast<std::chrono::nanoseconds>(
                    std::chrono::steady_clock::now().time_since_epoch())
                    .count());
}

}  // namespace time
}  // namespace common
}  // namespace sserver

#endif  // SSERVER_COMMON_TIME_MONOTONICCLOCK_H
