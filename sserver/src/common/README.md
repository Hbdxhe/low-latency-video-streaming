# common

## 1. 模块定位

无业务语义的基础能力层。提供日志、延迟统计、线程安全队列、公共数据模型、网络协议结构、时间工具等被所有模块共用的基础设施。

## 2. 核心职责

- 提供线程安全的日志系统（`log/Logger`）
- 提供延迟统计和百分位计算（`metrics/LatencyRecorder`）
- 提供线程安全队列模板（`concurrency/ThreadSafeQueue`）
- 定义核心数据模型 `EncodedFrame`（`model/`）
- 定义网络协议结构（`net/StreamProtocol`、`net/RtpProtocol`、`net/H264AnnexB`）
- 提供单调时钟工具（`time/MonotonicClock`）

## 3. 子目录说明

| 子目录 | 作用 |
|---|---|
| log/ | 日志系统，支持 Debug/Info/Warn/Error 四个级别，同时输出到 stdout/stderr 和日志文件 |
| metrics/ | 延迟记录器，支持纳秒级延迟记录和 min/avg/p50/p95/p99/max 百分位统计 |
| concurrency/ | 并发工具，提供 `ThreadSafeQueue` 模板，支持阻塞等待、丢弃最旧、选择性丢弃等策略 |
| model/ | 公共数据模型，定义 `EncodedFrame` 结构体和 `StreamPayloadType` 枚举 |
| net/ | 网络协议结构，定义 TCP/UDP 自定义协议头、RTP 协议头解析/序列化、H.264 Annex-B NALU 分割工具 |
| time/ | 时间工具，提供 `MonotonicNowNs()` 获取单调时钟纳秒时间戳 |

## 4. 与其他模块的关系

- 被 `app`、`core`、`config`、`modules` 所有层使用
- `common` 不依赖任何业务模块，只依赖 C++ 标准库和系统 API
- `EncodedFrame` 是 capture → transport 之间的核心数据载体
- `ThreadSafeQueue` 被 `CaptureModule` 和 `TcpClientSession` 使用
- `LatencyRecorder` 被 `AppBootstrap`（全局）和 `TcpClientSession`（per-client）使用
- `StreamProtocol` 被 TCP 和 UDP 传输后端使用
- `RtpProtocol` 和 `H264AnnexB` 被 RTP 传输后端使用
- `MonotonicClock` 被采集和传输模块广泛使用

## 5. 线程模型

`common` 层的组件设计考虑了多线程使用：
- `Logger` 内部使用 `std::mutex` 保护输出
- `LatencyRecorder` 内部使用 `std::mutex` 保护采样数据
- `ThreadSafeQueue` 内部使用 `std::mutex` + `std::condition_variable`

## 6. 后续扩展方向

- 增加结构化日志（JSON 格式）
- 增加日志级别动态调整
- 增加更多公共数据模型（如 AudioFrame）
- 增加更多协议支持（如 RTCP、SRT、WebRTC 信令）
- 增加内存池或环形缓冲区优化高频分配
