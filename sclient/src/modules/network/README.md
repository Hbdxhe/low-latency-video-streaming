# src/modules/network

## 1. 模块定位

网络接收模块。负责通过 TCP/UDP/RTP 协议接收视频流数据，处理分片重组、丢包恢复（NACK/FEC）、抖动缓冲等网络层问题，输出完整的 `ReceivedFrame` 给解码模块。

## 2. 核心职责

- TCP 流式接收：`MessageHeader` + `FrameDiagnosticMetadata` + payload
- UDP 分片接收：按 `UdpFrameFragmentHeader` 重组分片
- RTP 接收：解析 RTP 头，处理 FU-A 分片，重组 Annex B 码流
- NACK：检测缺失分片，发送重传请求
- FEC：XOR 奇偶校验恢复单个缺失分片
- 自适应抖动缓冲：根据网络质量动态调整缓冲延迟
- KeepAlive 心跳：定期发送心跳保活（TCP/UDP）
- UDP 接收统计：记录收包、丢包、抖动等指标

## 3. 主要文件说明

| 文件 | 作用 |
|---|---|
| `StreamClient.h` | 统一接收客户端接口。定义 `Connect()`、`ReceiveFrame()`、`Close()` 等方法 |
| `StreamClient.cpp` | 连接管理、KeepAlive 循环、UDP 统计快照 |
| `StreamClientInternal.h` | 内部工具函数：`BuildSocketError()`、`MonotonicNowNs()`、`XorInto()` |
| `StreamClientTcp.cpp` | TCP 接收实现：`ReceiveTcpFrame()`、`ReceiveAll()` |
| `StreamClientUdp.cpp` | UDP 接收实现：`ReceiveUdpFrame()`、`ProcessUdpDatagram()`、分片重组、NACK、FEC |
| `StreamClientRtp.cpp` | RTP 接收实现：`ReceiveRtpFrame()`、FU-A 重组、延迟扩展解析 |
| `AdaptiveJitterBuffer.h` | 自适应抖动缓冲：根据网络质量（excellent/good/fair/poor）自动切换模式 |
| `types/ClientConfig.h` | 客户端配置结构体，包含所有 UDP/jitter/NACK/FEC 参数 |
| `types/ReceivedFrame.h` | 接收到的帧数据结构 |
| `types/UdpReceiveStats.h` | UDP 接收统计结构体 |

## 4. 核心类 / 函数说明

### StreamClient

作用：
- 统一的网络接收客户端
- 根据 `config.transport` 自动选择 TCP/UDP/RTP 接收路径
- 管理 socket 生命周期

关键函数：
- `Connect(config, error)`：建立连接（TCP connect / UDP connect / RTP bind）
- `ReceiveFrame(frame, error)`：接收一帧，根据协议分发到对应实现
- `Close()`：关闭连接，停止 KeepAlive 线程
- `udp_receive_stats()`：获取 UDP 接收统计快照
- `BoundPort()`：获取本地绑定端口（RTP 模式使用）

内部方法：
- `ReceiveTcpFrame()`：TCP 流式接收，先读 header，再读 metadata + payload
- `ReceiveUdpFrame()`：UDP 循环接收，drain 所有可用数据报 → 处理 → 尝试弹出就绪帧
- `ReceiveRtpFrame()`：RTP 接收，解析 RTP 头 → FU-A 重组 → marker 位表示帧结束
- `ProcessUdpDatagram()`：处理单个 UDP 数据报：校验、分片入库、尝试 FEC 恢复、尝试完成帧
- `MaybeRecoverWithUdpFec()`：XOR FEC 恢复（恰好缺失 1 个分片时）
- `MaybeSendUdpNackRequests()`：检测缺失分片，发送 NACK
- `SendUdpNack()`：构造并发送 NACK 报文
- `BufferCompletedUdpFrame()`：完成的帧进入 jitter buffer
- `TryPopReadyUdpFrame()`：从 jitter buffer 弹出就绪帧
- `SendKeepAlive()`：发送心跳（TCP 空消息 / UDP 含 UdpReceiverReport）
- `KeepAliveLoop()`：心跳循环线程

### AdaptiveJitterBuffer

作用：
- 根据网络抖动自适应调整缓冲延迟
- 三种模式：bypass（无缓冲）、low_latency（低延迟）、smooth（平滑）

关键函数：
- `RecordJitter(jitter_ms, loss%, skip%, now_ns)`：记录抖动采样
- `TargetDelayNs()`：返回当前目标延迟
- `SetFixedMode(mode)`：设置固定模式
- `EnableAutoMode()`：启用自动模式

模式切换逻辑：
- bypass → low_latency：连续 10 次抖动 > 1ms
- low_latency → bypass：连续 20 次抖动 < 0.5ms
- low_latency → smooth：p95 > 10ms 且连续两次
- smooth → low_latency：p95 < 5ms 持续 3 秒

### ClientConfig

作用：
- 客户端配置结构体
- 包含网络地址、传输协议、UDP 参数、jitter buffer 参数、NACK/FEC 参数、测试注入参数

关键字段：
- `host` / `port` / `transport`：网络连接参数
- `udp_jitter_buffer_enabled` / `udp_jitter_buffer_strategy`：jitter buffer 配置
- `udp_nack_enabled` / `udp_fec_enabled`：丢包恢复配置
- `udp_test_loss_pattern` / `udp_test_jitter_pattern`：测试注入配置

### ReceivedFrame

作用：
- 接收到的帧数据结构
- 从网络层输出，进入 `BoundedQueue`

关键字段：
- `header`：`MessageHeader`
- `metadata`：`FrameDiagnosticMetadata`（延迟元数据）
- `sender_metadata_available`：发送端是否提供了延迟元数据
- `receive_timestamp_ns`：本地接收时间戳
- `payload`：帧载荷（H.264 Annex B 码流）

### UdpReceiveStats

作用：
- UDP 接收统计结构体
- 包含收包计数、丢包计数、抖动统计、jitter buffer 统计、NACK/FEC 统计

关键字段：
- `datagrams_received` / `fragments_received` / `completed_frames`：收包统计
- `timed_out_fragments` / `timed_out_frames`：超时统计
- `jitter_last_ms` / `jitter_avg_ms` / `jitter_max_ms` / `jitter_p50_ms` / `jitter_p95_ms`：抖动统计
- `jitter_buffer_emitted_frames` / `jitter_buffer_skipped_frames` / `jitter_buffer_dropped_frames`：jitter buffer 统计
- `nack_requests_sent` / `fec_recovered_frames`：恢复统计

## 5. 数据流说明

输入：
- TCP：服务端发送的 `MessageHeader` + `FrameDiagnosticMetadata` + H.264 payload
- UDP：服务端发送的 `MessageHeader` + `UdpFrameFragmentHeader` + 分片数据
- RTP：发送端的 RTP 包（`RtpHeader` + H.264 NALU / FU-A 分片）

处理：
- TCP：`ReceiveAll()` 流式读取 → 校验魔数 → 解析 metadata → 读取 payload
- UDP：`recv()` → `ProcessUdpDatagram()` → 校验 → 分片入库 → FEC 恢复 → 分片到齐后组装帧 → jitter buffer
- RTP：`recv()` → `ParseRtpHeader()` → NALU/FU-A 重组 → marker 位后组装帧 → jitter buffer（可选）

输出：
- `ReceivedFrame`：包含 header、metadata、payload、时间戳

## 6. 与其他模块的关系

```
modules/network/
  ├── 使用 common/protocol/Protocol.h 的协议结构体
  ├── 使用 common/net/RtpProtocol.h 解析 RTP
  ├── 使用 common/net/H264AnnexB.h 拼接 Annex B
  ├── 使用 common/log/Logger 输出日志
  ├── 使用 common/metrics/LatencyStats（AdaptiveJitterBuffer 内部）
  └── 输出 ReceivedFrame → 被 app/main.cpp 通过 BoundedQueue 传递给 decoding
```

## 7. 线程模型 / 队列模型

本模块涉及的线程：

| 线程 | 职责 |
|---|---|
| 接收线程（由 main.cpp 创建） | 调用 `ReceiveFrame()` 持续接收 |
| KeepAlive 线程（由 StreamClient 创建） | 定期发送心跳 |

线程安全：
- `send_mutex_`：保护 socket 发送操作（`send()`、`SendKeepAlive()`、`SendUdpNack()`）
- `udp_receive_stats_snapshot_mutex_`：保护 UDP 统计快照的读写
- `running_`：`std::atomic_bool`，控制 KeepAlive 线程退出

数据结构：
- `udp_assemblies_`（`map<uint64, UdpFrameAssembly>`）：按帧序号组织的分片组装状态
- `udp_jitter_buffer_`（`map<uint64, BufferedUdpFrame>`）：按帧序号组织的 jitter buffer
- `rtp_frame_assembly_`：RTP 帧组装状态（单帧）

丢帧策略：
- UDP 分片组装超时：250ms（`kUdpAssemblyTimeoutNs`）
- jitter buffer 满时丢弃新帧
- jitter buffer 超时后跳过缺失帧

## 8. 配置参数

### 网络参数

| 参数 | 默认值 | 说明 |
|---|---|---|
| `host` | `127.0.0.1` | 服务端地址 |
| `port` | `9999` | 端口 |
| `transport` | `tcp` | 传输协议 |
| `udp_max_datagram_size` | `65507` | UDP 最大报文大小 |
| `udp_receive_buffer_bytes` | `4MB` | UDP 接收缓冲区 |
| `keepalive_interval_ms` | `500` | 心跳间隔 |

### Jitter Buffer 参数

| 参数 | 默认值 | 说明 |
|---|---|---|
| `udp_jitter_buffer_enabled` | `true` | 是否启用 jitter buffer |
| `udp_jitter_buffer_strategy` | `auto` | 策略：auto/off/low/smooth/fixed/adaptive |
| `udp_jitter_buffer_target_delay_ms` | `8` | 目标延迟（fixed/adaptive 模式） |
| `udp_jitter_buffer_min_delay_ms` | `2` | 最小延迟 |
| `udp_jitter_buffer_safety_factor` | `1.5` | 安全系数 |
| `udp_jitter_buffer_max_wait_ms` | `40` | 最大等待时间 |
| `udp_jitter_buffer_max_frames` | `4` | 最大缓冲帧数 |

### NACK/FEC 参数

| 参数 | 默认值 | 说明 |
|---|---|---|
| `udp_nack_enabled` | `true` | 是否启用 NACK |
| `udp_fec_enabled` | `true` | 是否启用 FEC |
| `udp_nack_request_delay_ms` | `25` | NACK 请求延迟 |
| `udp_nack_retry_interval_ms` | `20` | NACK 重试间隔 |
| `udp_nack_max_retries` | `3` | 最大重试次数 |

### 测试注入参数

| 参数 | 默认值 | 说明 |
|---|---|---|
| `udp_test_loss_pattern` | `none` | 丢包模式：none/single/burst/alternate |
| `udp_test_jitter_pattern` | `none` | 抖动模式：none/saw/burst/alternate |
| `udp_test_jitter_amplitude_ms` | `0` | 抖动幅度 |

## 9. 调试建议

- **TCP 连接失败**：检查 `connect()` 返回的 errno，查看日志中的 `failed to connect`
- **UDP 收不到数据**：检查 `recv()` 是否返回 EAGAIN，查看 `datagrams_received` 统计
- **UDP 分片超时**：查看 `timed_out_fragments` 和 `timed_out_frames`，可能是网络丢包或分片大小不匹配
- **RTP 帧损坏**：查看 `frame_damaged` 标志，可能是序列号不连续或 FU-A 分片丢失
- **Jitter Buffer 积压**：查看 `jitter_buffer_current_depth`，如果持续增长说明消费速度跟不上
- **NACK 效果**：查看 `nack_requests_sent` 和 `fec_recovered_frames`
- **适合打断点的位置**：
  - `ProcessUdpDatagram()`：观察每个数据报的处理
  - `FinalizeCompletedUdpFrame()`：观察帧完成
  - `TryPopReadyUdpFrame()`：观察 jitter buffer 弹出逻辑
  - `MaybeRecoverWithUdpFec()`：观察 FEC 恢复

## 10. 后续扩展方向

- 增加 SRT 传输协议
- 增加 WebRTC 支持
- 增加 QUIC 传输
- 增加拥塞控制（带宽估计）
- 增加更完善的 FEC（Reed-Solomon）
- 增加多流复用
- 增加连接池
- 增加 IPv6 支持
