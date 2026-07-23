# tests/support

## 1. 模块定位

测试辅助工具。提供 TCP/UDP 帧接收、KeepAlive 发送、NACK 发送、UDP 分片重组、FEC 恢复等公共函数，被集成测试和基准测试复用。

## 2. 主要文件说明

| 文件 | 作用 |
|---|---|
| TransportTestClient.h | header-only 测试辅助库，提供 TCP/UDP 客户端操作的公共函数 |

## 3. 核心结构体

### ReceivedFrame

```cpp
struct ReceivedFrame {
    MessageHeader header;              // 消息头
    FrameDiagnosticMetadata metadata;  // 帧诊断元数据
    uint64_t receive_timestamp_ns;     // 接收时间戳
    vector<uint8_t> payload;           // 帧 payload
};
```

### UdpFrameAssemblyState

UDP 分片重组状态：
- `header` / `metadata`：从分片头中提取
- `payload`：重组后的完整帧数据
- `received_fragments`：各分片是否已收到
- `received_fragment_count`：已收到分片数
- `fec_payload` / `has_fec_payload`：FEC 奇偶校验数据

## 4. 核心函数

### 连接管理

- `WaitForBoundPort(bootstrap, port)`：轮询等待 `bootstrap.bound_port()` 就绪（最多 50 次，每次 20ms）
- `ConnectClient(transport_config, port, socket_fd)`：创建 TCP/UDP socket 并 connect，UDP 模式下自动发送 KeepAlive

### TCP 帧接收

- `ReceiveTcpFrame(socket_fd, expect_metadata, frame)`：接收 `MessageHeader` + 可选 `FrameDiagnosticMetadata` + payload

### UDP 分片重组

- `ReceiveUdpFrame(socket_fd, max_datagram_size, expect_metadata, frame)`：循环接收 UDP 数据报，重组为完整帧
- `ReceiveUdpFrameWithNackRecovery(...)`：重组时故意丢弃第一个数据分片，发送 NACK 请求重传，验证恢复
- `ReceiveUdpFrameWithFecRecovery(...)`：重组时故意丢弃第一个数据分片，等待 FEC 奇偶校验分片到达后恢复
- `ConsumeUdpDatagram(...)`：处理单个 UDP 数据报，更新重组状态
- `MaybeRecoverUdpAssemblyWithFec(assembly)`：尝试用 FEC 恢复单个丢失分片
- `TryFinalizeUdpAssembly(frame_sequence, assemblies, frame)`：检查分片是否全部到达，组装完整帧

### 协议操作

- `SendKeepAlive(socket_fd)`：发送 KeepAlive 消息（带 `UdpReceiverReport`）
- `SendUdpNack(socket_fd, items)`：发送 NACK 请求
- `RefreshClientKeepAlive(socket_fd, transport_config, frame_index)`：UDP 模式下每 15 帧发送一次 KeepAlive

### 统一帧接收

- `ReceiveFrame(socket_fd, transport_config, frame)`：根据 backend 类型分发到 `ReceiveTcpFrame` 或 `ReceiveUdpFrame`

## 5. 与其他模块的关系

- 被 `TransportIntegrationTest.cpp` 使用
- 被 `TransportBenchmark.cpp` 使用
- 被 `UdpFecRecoveryTest.cpp` 使用（测试 FEC 恢复逻辑）
- 依赖 `common/net/StreamProtocol.h` 的协议结构
- 依赖 `common/time/MonotonicClock.h` 获取时间戳
