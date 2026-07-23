# tests/integration

## 1. 模块定位

集成测试目录。使用本地 loopback socket 进行真实的网络收发，验证 TCP/UDP/RTP 接收链路的端到端行为。

## 2. 核心职责

- 验证 TCP 帧接收和 metadata 传递
- 验证 UDP jitter buffer 跳帧逻辑
- 验证 UDP FEC（XOR 奇偶校验）恢复
- 验证 UDP NACK 重传恢复
- 验证 RTP Single NALU 和 FU-A 重组
- 验证 RTP 损坏帧丢弃和重同步
- 验证 RTP 延迟扩展恢复和降级

## 3. 主要文件说明

| 文件 | 测试内容 |
|---|---|
| `TcpReceiveIntegrationTest.cpp` | TCP loopback：服务端发送一帧 → 客户端接收 → 验证 metadata 和 payload |
| `UdpReceiveIntegrationTest.cpp` | UDP loopback：jitter buffer 跳帧、FEC 恢复、NACK 恢复 |
| `RtpReceiveIntegrationTest.cpp` | RTP loopback：Single NALU + FU-A 重组、损坏帧丢弃、延迟扩展恢复/降级 |

## 4. 核心测试函数说明

### TcpReceiveIntegrationTest

- `TestTcpFrameMarksSenderMetadataAvailable()`：TCP 发送含 metadata 的帧 → 接收 → 验证 `sender_metadata_available`、sequence、capture_timestamp、transport_send_timestamp、payload 一致

### UdpReceiveIntegrationTest

- `TestJitterBufferSkipsMissingFrame()`：发送帧 1 和帧 3（跳过帧 2）→ 验证 jitter buffer 跳过帧 2，统计 `jitter_buffer_skipped_frames == 1`
- `TestFecRecovery()`：发送 2 个数据分片 + 1 个 XOR 校验分片（缺失 1 个数据分片）→ 验证 FEC 恢复，统计 `fec_recovered_frames == 1`
- `TestNackRecovery()`：只发送 1 个分片 → 等待客户端发送 NACK → 服务端补发缺失分片 → 验证帧完整，统计 `nack_requests_sent >= 1`

### RtpReceiveIntegrationTest

- `TestReassemblesSingleNaluAndFuAFrame()`：发送 SPS + PPS + IDR（FU-A 分片）→ 验证重组为 Annex B 码流
- `TestDropsDamagedFrameAndResyncs()`：发送序列号不连续的 FU-A（模拟丢包）→ 发送下一帧 → 验证损坏帧被丢弃，下一帧正常接收
- `TestRestoresSenderMetadataFromRtpExtension()`：发送带延迟扩展的 RTP 包 → 验证 capture_timestamp 和 transport_send_timestamp 正确恢复
- `TestFallsBackWhenLatencyExtensionIsMissing()`：发送无延迟扩展的 RTP 包 → 验证 `sender_metadata_available == false`
- `TestFallsBackWhenLatencyExtensionIsInvalid()`：发送无效延迟扩展 → 验证降级为无 metadata

## 5. 测试基础设施

### TcpLoopbackServer

- 绑定本地端口 → listen → accept → 发送帧
- 被 `TcpReceiveIntegrationTest` 使用

### UdpLoopbackServer

- 绑定本地端口 → 等待客户端首包 → 记录客户端地址 → 发送数据报
- 支持 `WaitForClient()` 和 `ReceiveMessageOfType()`（用于捕获 NACK）
- 被 `UdpReceiveIntegrationTest` 使用

### RtpLoopbackServer

- 创建 UDP socket → 向指定地址/端口发送 RTP 包
- 被 `RtpReceiveIntegrationTest` 使用

### 辅助函数

- `BuildUdpFragmentPacket()`：构造 UDP 分片数据报
- `BuildXorParity()`：计算 XOR 奇偶校验
- `BuildRtpPacket()` / `BuildSingleNaluPacket()` / `BuildFuAPacket()`：构造 RTP 包

## 6. 与其他模块的关系

- 调用 `modules/network/StreamClient` 进行接收
- 使用 `common/protocol/Protocol.h` 的协议结构体
- 使用 `common/net/RtpProtocol.h` 和 `common/net/H264AnnexB.h`
- 使用 `tests/support/` 的断言和 socket 工具

## 7. 运行方式

```bash
ctest --test-dir build --output-on-failure -L integration
```

超时限制：TCP 10 秒，UDP 15 秒，RTP 10 秒。

UDP 测试支持选择性运行：
```bash
./build/udp_receive_integration_test jitter   # 只跑 jitter buffer 测试
./build/udp_receive_integration_test fec       # 只跑 FEC 测试
./build/udp_receive_integration_test nack      # 只跑 NACK 测试
./build/udp_receive_integration_test all       # 全部（默认）
```
