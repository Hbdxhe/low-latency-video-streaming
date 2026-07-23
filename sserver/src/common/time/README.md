# common/time

## 1. 模块定位

时间工具。提供基于单调时钟的纳秒级时间戳获取。

## 2. 核心职责

- 提供 `MonotonicNowNs()` 函数，返回单调时钟的纳秒时间戳
- 单调时钟不受系统时间调整影响，适合延迟计算

## 3. 主要文件说明

| 文件 | 作用 |
|---|---|
| MonotonicClock.h | 定义 `MonotonicNowNs()` 内联函数，header-only 实现 |

## 4. 核心函数说明

### MonotonicNowNs()

```cpp
inline uint64_t MonotonicNowNs() {
    return static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(
            std::chrono::steady_clock::now().time_since_epoch()
        ).count());
}
```

- 使用 `std::chrono::steady_clock`（单调时钟）
- 返回自时钟纪元以来的纳秒数
- 不受系统时间修改（如 NTP 调整）影响

## 5. 与其他模块的关系

- 被 `NullCaptureDevice`：记录采集时间戳
- 被 `V4L2CaptureDevice`：记录采集时间戳（fallback）和编码时间戳
- 被 `TcpClientSession`：记录入队时间和发送时间
- 被 `UdpStreamingBackend`：记录发送时间戳和客户端最后活跃时间
- 被 `RtpStreamingBackend`：记录发送时间戳
- 被 `LatencyRecorder`：计算延迟差值
