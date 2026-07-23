# modules/transport

## 1. 模块定位

流传输模块。负责将编码后的 `EncodedFrame` 通过 TCP、UDP 或 RTP 协议发送给客户端。采用「门面 + backend 接口 + 工厂」模式，支持三种传输后端。

## 2. 核心职责

- 提供 `ITransportBackend` 接口，抽象传输后端能力
- 提供 `Transport` 门面类，封装后端创建和生命周期管理
- 提供 `TransportModule`（`IModule` 实现），作为传输模块的生命周期管理入口
- 实现 `TcpStreamingBackend`：TCP 传输，支持多客户端、per-client 发送线程、队列背压
- 实现 `UdpStreamingBackend`：UDP 传输，支持分片、FEC、NACK 重传、客户端自动发现
- 实现 `RtpStreamingBackend`：RTP 传输，支持 H.264 FU-A 分片、延迟扩展头、Pacing 发送

## 3. 主要文件说明

| 文件 | 作用 |
|---|---|
| ITransportBackend.h | 定义 `ITransportBackend` 纯虚接口：`initialize`、`start`、`stop`、`Broadcast` 等 |
| Transport.h / .cpp | `Transport` 门面类，封装后端创建（`TransportBackendFactory`）和生命周期管理 |
| TransportModule.h / .cpp | `TransportModule`（`IModule` 实现），代理到 `Transport` 门面 |
| tcp/TcpStreamingBackend.h / .cpp | TCP 传输后端：监听端口、accept 新连接、管理客户端会话列表 |
| tcp/TcpClientSession.h / .cpp | TCP 客户端会话：per-client 的收发线程、发送队列、背压控制、延迟统计 |
| udp/UdpStreamingBackend.h / .cpp | UDP 传输后端：分片发送、FEC 奇偶校验、NACK 重传缓存、客户端自动发现/超时清理 |
| rtp/RtpPacketizer.h / .cpp | RTP 打包器：将 H.264 Annex-B 数据分割为 RTP 包（单 NALU 或 FU-A 分片） |
| rtp/RtpStreamingBackend.h / .cpp | RTP 传输后端：UDP socket 发送、Pacing 控制、SDP 文件生成、延迟扩展头 |

## 4. 核心类 / 函数说明

### ITransportBackend

作用：
- 传输后端的抽象接口
- 当前有三个实现：`TcpStreamingBackend`、`UdpStreamingBackend`、`RtpStreamingBackend`

关键函数：
- `initialize(context)`：从上下文获取配置并初始化
- `start()`：启动后端（如开始监听、启动接收线程）
- `Broadcast(frame)`：将编码帧发送给所有客户端
- `bound_port()`：返回实际监听的端口号

### TransportModule

作用：
- 实现 `IModule` 接口，作为传输模块的生命周期管理入口
- 代理到 `Transport` 门面

关键函数：
- `initialize(context)`：初始化传输后端
- `start()`：启动传输后端
- `Broadcast(frame)`：广播编码帧给所有客户端
- `bound_port()`：返回监听端口

### TcpStreamingBackend

作用：
- TCP 传输后端
- 监听指定端口，accept 新连接
- 为每个连接创建 `TcpClientSession`
- `Broadcast()` 时将帧推送到所有客户端的发送队列

关键函数：
- `OpenListenSocket()`：创建 TCP socket、bind、listen
- `AcceptLoop()`：accept 新连接，创建 `TcpClientSession` 并启动
- `Broadcast(frame)`：遍历客户端列表，调用 `session->EnqueueFrame(frame)`
- `PruneClosedClients()`：清理已断开的客户端

### TcpClientSession

作用：
- 单个 TCP 客户端的会话管理
- 独立的接收线程（检测 KeepAlive/断连）和发送线程（从队列取帧发送）
- 支持队列背压控制和丢帧策略

关键函数：
- `Start()`：启动接收线程和发送线程
- `EnqueueFrame(frame)`：将帧入队，支持 `drop_oldest` 和 `drop_oldest_non_key` 两种丢弃策略
- `ReceiveLoop()`：select 等待可读事件，接收 KeepAlive 消息，超时断连
- `SendLoop()`：从队列阻塞取帧，检查帧是否过期（`max_queue_wait_ms`），使用 `sendmsg` + scatter/gather IO 发送
- `SendFrame()`：构建 `MessageHeader` + 可选 `FrameDiagnosticMetadata` + payload，调用 `SendMessageParts()`
- `SendMessageParts()`：使用 `sendmsg` + `iovec` 实现多段发送

队列丢弃策略：
- `drop_oldest`：队满时丢弃最旧帧
- `drop_oldest_non_key`：队满时优先丢弃非关键帧，如果全是关键帧则丢弃最旧

### UdpStreamingBackend

作用：
- UDP 传输后端
- 支持大帧自动分片（基于 `udp_target_payload_size`）
- 支持 XOR FEC 奇偶校验（每帧一个奇偶校验分片）
- 支持 NACK 重传（客户端请求重传丢失的分片）
- 支持客户端自动发现（通过 KeepAlive 包）和超时清理
- 支持 `sendmmsg` 批量发送（多客户端时）

关键函数：
- `OpenSocket()`：创建 UDP socket、设置缓冲区大小、bind
- `ReceiveLoop()`：select 等待可读事件，接收 KeepAlive 和 NACK 包
- `Broadcast(frame)`：获取客户端快照 → `SendFrameFragments()` → 记录延迟统计
- `SendFrameFragments(frame, clients)`：
  1. 计算分片数量和大小
  2. 为每个分片构建 `MessageHeader` + `UdpFrameFragmentHeader` + payload
  3. 如果启用 NACK，缓存分片数据
  4. 如果启用 FEC，对所有分片 payload 做 XOR
  5. 单客户端用 `sendto`，多客户端用 `sendmmsg`
  6. 如果启用 FEC，额外发送一个奇偶校验分片
- `HandleNackRequest()`：从重传缓存中查找请求的分片并重发

### RtpPacketizer

作用：
- 将 H.264 Annex-B 数据打包为 RTP 包
- 小于 `rtp_max_payload_size` 的 NALU 直接打包
- 大于的使用 FU-A（Fragmentation Unit Type A）分片
- 支持自定义延迟扩展头（profile 0x5353）

关键函数：
- `Packetize(frame, packets, error_message)`：
  1. 使用 `SplitAnnexBNalus()` 分割 H.264 数据为 NALU 列表
  2. 构建 RTP 时间戳（基于采集时间戳和 clock rate）
  3. 为每个 NALU 调用 `PacketizeSingleNalu()` 或 `PacketizeFragmentedNalu()`
  4. 最后一个 NALU 的最后一个 RTP 包设置 marker bit

### RtpStreamingBackend

作用：
- RTP 传输后端
- 使用 UDP socket 发送 RTP 包到指定远程地址
- 支持 Pacing 发送（控制包间间隔，避免突发）
- 自动生成 SDP 文件供播放器使用

关键函数：
- `OpenSocket()`：创建 UDP socket、bind
- `ConfigureRemoteAddress()`：解析远程地址
- `WriteSdpFile()`：生成 SDP 文件（描述 H.264 流信息）
- `Broadcast(frame)`：
  1. `RtpPacketizer::Packetize()` 打包
  2. 计算 Pacing 间隔
  3. 逐包发送，每个包就地更新延迟扩展头的发送时间戳
- `ComputePacingInterval()`：基于帧间隔和包数量计算包间发送间隔
- `PacePacketBurst()`：使用 `clock_nanosleep` 精确控制发送时机

Pacing 策略：
- 帧内包数 < 4：不 pacing
- pacing 窗口 = min(2ms, 帧间隔/4)
- 包间间隔 = pacing 窗口 / (包数-1)
- 最小 pacing 间隔 50us

## 5. 数据流说明

### TCP 传输

```
TransportModule::Broadcast(frame)
  → Transport::Broadcast(frame)
  → TcpStreamingBackend::Broadcast(frame)
  → 遍历 clients_ → TcpClientSession::EnqueueFrame(frame)
  → ThreadSafeQueue (per-client)
  → SendLoop 线程 → SendFrame → sendmsg(iovec)
  → TCP Client
```

### UDP 传输

```
TransportModule::Broadcast(frame)
  → Transport::Broadcast(frame)
  → UdpStreamingBackend::Broadcast(frame)
  → SnapshotClients() → SendFrameFragments()
  → 构建 MessageHeader + UdpFrameFragmentHeader + payload
  → sendto / sendmmsg (多客户端)
  → 可选: FEC 奇偶校验分片
  → 可选: 缓存分片用于 NACK 重传
  → UDP Client
```

### RTP 传输

```
TransportModule::Broadcast(frame)
  → Transport::Broadcast(frame)
  → RtpStreamingBackend::Broadcast(frame)
  → RtpPacketizer::Packetize(frame)
    → SplitAnnexBNalus() → NALU 列表
    → 单 NALU: PacketizeSingleNalu()
    → 大 NALU: PacketizeFragmentedNalu() (FU-A)
  → Pacing 逐包发送
    → UpdateRtpLatencyExtensionTransportSendTimestamp()
    → sendto()
  → RTP Client (ffplay / VLC)
```

## 6. 与其他模块的关系

- 被 `AppBootstrap` 创建和注册为 `IModule`
- 被 `AppBootstrap::BindStreamingPipeline()` 通过 `FrameHandler` 回调触发 `Broadcast()`
- 接收 `CaptureModule` 输出的 `EncodedFrame`
- 使用 `common/net/StreamProtocol` 的协议结构（TCP/UDP）
- 使用 `common/net/RtpProtocol` 的 RTP 协议工具（RTP）
- 使用 `common/net/H264AnnexB` 的 NALU 分割工具（RTP）
- 使用 `common/metrics/LatencyRecorder` 记录延迟
- 使用 `common/concurrency/ThreadSafeQueue` 管理 TCP 发送队列
- 使用 `common/time/MonotonicClock` 获取时间戳
- 从 `config/AppConfig` 读取 `TransportConfig`

## 7. 线程模型 / 队列模型

### TCP 后端线程模型

```
AcceptLoop 线程 (TcpStreamingBackend)
  │
  ├─ accept() 新连接
  │    └─ 创建 TcpClientSession
  │         ├─ ReceiveLoop 线程: select → recv (KeepAlive)
  │         └─ SendLoop 线程: WaitPopFor → sendmsg
  │              └─ outbound_frames_ (ThreadSafeQueue, per-client)
  │
  └─ Broadcast() 在调用线程执行
       └─ 遍历 clients_ → EnqueueFrame() → PushDropOldest/PushDropSelective
```

- `AcceptLoop`：独立线程，accept 新连接
- 每个客户端：2 个线程（ReceiveLoop + SendLoop）
- `outbound_frames_`：per-client 的 `ThreadSafeQueue<QueuedFrame>`
- `clients_mutex_`：保护客户端列表

### UDP 后端线程模型

```
ReceiveLoop 线程 (UdpStreamingBackend)
  │
  ├─ select → recvfrom (KeepAlive / NACK)
  │    ├─ RegisterClient() → 更新 clients_ 列表
  │    └─ HandleNackRequest() → 从 retransmit_cache_ 重发分片
  │
  └─ Broadcast() 在调用线程执行
       └─ SnapshotClients() → SendFrameFragments() → sendto / sendmmsg
```

- `ReceiveLoop`：独立线程，接收客户端反馈
- `Broadcast()`：在采集/编码线程中同步执行
- `clients_mutex_`：保护客户端列表
- `retransmit_cache_mutex_`：保护重传缓存

### RTP 后端线程模型

```
Broadcast() 在调用线程同步执行
  → RtpPacketizer::Packetize()
  → 逐包 sendto() (带 Pacing)
```

- 无线程管理，完全同步
- Pacing 使用 `clock_nanosleep` 阻塞等待

## 8. 配置参数

### 通用参数

| 参数 | 说明 | 默认值 |
|---|---|---|
| transport.enabled | 是否启用传输 | true |
| transport.backend | 传输后端 | "tcp" |
| transport.bind_address | 监听地址 | "0.0.0.0" |
| transport.listen_port | 监听端口 | 9999 |
| transport.embed_frame_metadata | 是否嵌入帧诊断元数据 | false |

### TCP 参数

| 参数 | 说明 | 默认值 |
|---|---|---|
| transport.max_pending_frames | 发送队列最大帧数 | 3 |
| transport.max_queue_wait_ms | 帧最大队列等待时间 | 50 |
| transport.queue_drop_policy | 丢帧策略 | "drop_oldest_non_key" |
| transport.accept_loop_interval_ms | accept 循环间隔 | 5 |
| transport.enable_nodelay | TCP_NODELAY | true |

### UDP 参数

| 参数 | 说明 | 默认值 |
|---|---|---|
| transport.udp_client_timeout_ms | 客户端超时 | 5000 |
| transport.udp_max_datagram_size | 最大 UDP 数据报大小 | 65507 |
| transport.udp_target_payload_size | 目标分片 payload 大小 | 65000 |
| transport.udp_receive_buffer_bytes | 接收缓冲区 | 4MB |
| transport.udp_send_buffer_bytes | 发送缓冲区 | 4MB |
| transport.udp_enable_nack | 启用 NACK | false |
| transport.udp_enable_fec | 启用 FEC | false |
| transport.udp_retransmit_cache_frames | 重传缓存帧数 | 32 |
| transport.udp_retransmit_cache_max_age_ms | 重传缓存最大年龄 | 500 |
| transport.udp_retransmit_max_fragments_per_request | NACK 请求最大分片数 | 16 |

### RTP 参数

| 参数 | 说明 | 默认值 |
|---|---|---|
| transport.rtp_remote_host | 远程地址 | "127.0.0.1" |
| transport.rtp_remote_port | 远程端口 | 5004 |
| transport.rtp_payload_type | RTP payload type | 96 |
| transport.rtp_clock_rate | RTP clock rate | 90000 |
| transport.rtp_ssrc | RTP SSRC | 305419896 |
| transport.rtp_max_payload_size | RTP 最大 payload | 1200 |
| transport.rtp_enable_latency_extension | 启用延迟扩展头 | true |
| transport.rtp_sdp_path | SDP 文件路径 | "sserver_rtp.sdp" |

## 9. 调试建议

### TCP 调试
- 检查 "tcp client connected" 日志确认客户端连接
- 检查 "transport latency" 日志查看端到端延迟统计
- 检查 "tcp queue stats" 日志查看队列深度、丢帧数、背压事件
- 如果客户端频繁断连，检查 "client keepalive timeout" 日志

### UDP 调试
- 检查 "udp client registered" 日志确认客户端注册
- 检查 "udp transport stats" 日志查看发送帧数、分片数、NACK 统计
- 检查 "udp client report" 日志查看客户端反馈的丢包率和 jitter
- 如果丢帧严重，尝试调整 `udp_target_payload_size` 和缓冲区大小

### RTP 调试
- 检查 "rtp transport stats" 日志查看发送帧数和失败数
- 使用 `ffplay` + SDP 文件验证：`ffplay -protocol_whitelist file,udp,rtp -i sserver_rtp.sdp`
- 如果播放卡顿，检查 Pacing 策略和网络状况
- 延迟扩展头可在客户端侧计算端到端延迟

## 10. 后续扩展方向

- 增加 RTCP 反馈支持
- 增加 SRT 传输后端
- 增加 WebRTC 传输后端
- 增加自适应码率（基于客户端反馈）
- 增加更完善的 FEC（Reed-Solomon）
- 增加多播支持
- 将 RTP Pacing 升级为更精确的发送调度器
