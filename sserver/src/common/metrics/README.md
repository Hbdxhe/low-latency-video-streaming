# common/metrics

## 1. 模块定位

延迟统计模块。提供纳秒级延迟记录和百分位统计能力，用于监控从采集到发送的端到端延迟。

## 2. 核心职责

- 记录纳秒级延迟数据
- 计算 min/avg/p50/p95/p99/max 百分位统计
- 提供格式化的统计摘要字符串

## 3. 主要文件说明

| 文件 | 作用 |
|---|---|
| LatencyRecorder.h | 定义 `LatencyRecorder` 类和 `LatencySummary` 结构体 |
| LatencyRecorder.cpp | 实现延迟记录、滑动窗口管理、百分位计算 |

## 4. 核心类 / 函数说明

### LatencyRecorder

作用：
- 内部维护一个 `std::deque<double>` 作为滑动窗口存储（单位：毫秒）
- 默认最大采样数 4096，超过时丢弃最旧的采样
- 线程安全，内部使用 `std::mutex`

关键函数：
- `RecordNs(uint64_t latency_ns)`：记录一个纳秒级延迟值
- `Snapshot()`：返回当前采样窗口的统计快照
- `Format(name)`：返回格式化的统计字符串，如 `capture_to_send count=120 min=1.23ms avg=2.45ms p50=2.10ms p95=3.50ms p99=4.00ms max=5.67ms`

### LatencySummary

- `count`：采样数
- `min_ms` / `avg_ms` / `p50_ms` / `p95_ms` / `p99_ms` / `max_ms`：各项统计指标

## 5. 数据流说明

输入：
- 纳秒级延迟值（通过 `RecordNs()` 写入）

处理：
- 转换为毫秒存入滑动窗口
- `Snapshot()` 时对窗口内数据排序后计算百分位

输出：
- `LatencySummary` 统计结构体
- 格式化字符串

## 6. 与其他模块的关系

- 被 `AppBootstrap` 创建一个全局实例，用于记录 capture-to-send 端到端延迟
- 被 `TcpClientSession` 创建 per-client 实例，用于记录队列等待时间和发送耗时
- 被 `TransportModule` / `Transport` / 各 `ITransportBackend` 传递和使用

## 7. 线程模型

`RecordNs()` 和 `Snapshot()` 内部都使用 `std::mutex` 保护。多线程并发调用是安全的。

注意：`Snapshot()` 会复制整个 `deque` 后在锁外排序，避免长时间持锁。

## 8. 配置参数

- `max_samples`：最大采样数（构造函数参数，默认 4096）
- `runtime.latency_log_interval_frames`：每隔多少帧输出一次延迟统计日志（配置参数，默认 120）
