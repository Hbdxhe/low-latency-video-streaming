# src/common

## 1. 模块定位

公共基础层。提供被所有业务模块共享的基础设施：日志、协议定义、并发队列、延迟统计、网络工具和媒体数据结构。本模块不包含业务逻辑，只提供底层能力。

## 2. 核心职责

- 提供统一的日志能力（控制台 + 文件双输出）
- 定义应用层协议格式（消息头、UDP 分片头、NACK 协议）
- 提供线程安全的有界队列
- 提供延迟统计工具（环形缓冲、百分位计算）
- 提供 H.264 Annex B 拼接和 IDR 检测
- 提供 RTP 协议解析（头部、扩展、延迟扩展）
- 提供 SDP 文件解析
- 定义解码后帧的数据结构

## 3. 子目录说明

| 子目录 | 作用 |
|---|---|
| `concurrency/` | 并发工具，当前包含 `BoundedQueue` 有界线程安全队列 |
| `log/` | 日志模块，`Logger` 类提供 Debug/Info/Warn/Error 四级日志 |
| `media/` | 媒体数据结构，`DecodedFrame` 描述解码后的帧 |
| `metrics/` | 度量工具，`LatencyStats` 提供环形缓冲延迟统计 |
| `net/` | 网络工具，包含 H.264 Annex B 工具、RTP 协议、SDP 解析 |
| `protocol/` | 应用层协议定义，`Protocol.h` 定义消息格式 |

## 4. 主要文件说明

| 文件 | 作用 |
|---|---|
| `concurrency/BoundedQueue.h` | 模板有界队列，支持 `PushOrDropOldest`、`WaitPop`、`TryPop`、`Close` |
| `log/Logger.h` | 日志接口声明 |
| `log/Logger.cpp` | 日志实现：控制台输出 + `logs/` 目录下文件输出，线程安全 |
| `media/DecodedFrame.h` | `DecodedFrame` 结构体：width/height/pixel_format/data/linesize/owner |
| `metrics/LatencyStats.h` | 环形缓冲延迟统计：Record/Snapshot/Format，支持 min/avg/p50/p95/p99/max |
| `net/H264AnnexB.h` | H.264 Annex B 工具：`AppendAnnexBNalu()` 拼接 NALU，`HasIdrNalUnit()` 检测 IDR |
| `net/RtpProtocol.h` | RTP 协议：`ParseRtpHeader()`、`WriteRtpHeader()`、延迟扩展序列化/反序列化 |
| `net/SdpSessionDescription.h` | SDP 解析接口：`ParseRtpVideoSessionDescription()`、`LoadRtpVideoSessionDescription()` |
| `net/SdpSessionDescription.cpp` | SDP 解析实现：提取 connection address、video port、payload type、clock rate |
| `protocol/Protocol.h` | 应用层协议：`MessageHeader`、`FrameDiagnosticMetadata`、`UdpFrameFragmentHeader`、`UdpNackHeader`、`UdpReceiverReport` |

## 5. 数据流说明

本模块是基础设施层，不直接参与数据管线流转，而是被其他模块调用：

- `protocol/Protocol.h`：被 `StreamClient` 和 `main.cpp` 使用，定义了线路上传输的所有数据格式
- `concurrency/BoundedQueue.h`：被 `main.cpp` 使用，在接收线程和解码线程之间传递帧
- `media/DecodedFrame.h`：被 `VideoDecoder` 输出，被 `VideoRenderer` 输入
- `metrics/LatencyStats.h`：被 `main.cpp` 和 `AdaptiveJitterBuffer` 使用
- `net/H264AnnexB.h`：被 `StreamClientRtp` 和 `main.cpp` 使用
- `net/RtpProtocol.h`：被 `StreamClientRtp` 使用
- `net/SdpSessionDescription.h`：被 `main.cpp` 使用，解析 SDP 文件

## 6. 与其他模块的关系

```
common/
  ├── protocol/Protocol.h     ← 被 modules/network/ 和 app/ 使用
  ├── concurrency/BoundedQueue.h ← 被 app/main.cpp 使用
  ├── media/DecodedFrame.h    ← 被 modules/decoding/ 输出，modules/rendering/ 输入
  ├── metrics/LatencyStats.h  ← 被 app/main.cpp 和 modules/network/AdaptiveJitterBuffer 使用
  ├── net/H264AnnexB.h        ← 被 modules/network/StreamClientRtp 和 app/main.cpp 使用
  ├── net/RtpProtocol.h       ← 被 modules/network/StreamClientRtp 使用
  ├── net/SdpSessionDescription ← 被 app/main.cpp 使用
  └── log/Logger              ← 被所有模块使用
```

## 7. 线程模型 / 队列模型

- `BoundedQueue`：本模块提供的核心并发原语。内部使用 `std::mutex` + `std::condition_variable`，支持阻塞等待（`WaitPop`）和非阻塞尝试（`TryPop`）。`PushOrDropOldest` 在队列满时丢弃最旧元素，避免生产者阻塞。`Close()` 唤醒所有等待者并标记队列关闭。
- `Logger`：内部使用 `std::mutex` 保证日志输出的原子性，使用 `std::atomic<bool>` 控制 verbose 开关。
- `LatencyStats`：非线程安全，由调用方保证（在 `main.cpp` 中仅在渲染线程访问）。

## 8. 配置参数

本模块不直接管理配置参数。相关配置通过上层模块传入：
- `Logger::SetVerbose(bool)`：控制是否输出 DEBUG 级别日志
- `BoundedQueue` 构造参数：队列容量
- `LatencyStats` 构造参数：最大采样数（默认 4096）、快照刷新间隔（默认 8）

## 9. 调试建议

- **日志不输出到文件**：检查 `logs/` 目录是否可创建，日志文件名格式为 `{程序名}_{日期}_{PID}.log`
- **协议解析失败**：检查 `HasValidMessageMagic()` 返回值，确认魔数为 `CCTC`
- **SDP 解析失败**：检查 SDP 文件是否包含 `c=IN IP4`、`m=video`、`a=rtpmap:` 行
- **适合打断点的位置**：
  - `Logger::Log()`：所有日志的统一出口
  - `ParseRtpVideoSessionDescription()`：SDP 解析入口
  - `BoundedQueue::PushOrDropOldest()`：观察队列丢帧

## 10. 后续扩展方向

- 增加结构化日志（JSON 格式）
- 增加日志级别运行时动态调整
- 增加更多协议定义（RTCP、WebRTC signaling）
- 增加线程安全的度量统计（当前 `LatencyStats` 非线程安全）
- 增加内存池或环形缓冲区减少分配
