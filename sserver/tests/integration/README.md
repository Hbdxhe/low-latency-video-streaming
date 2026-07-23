# tests/integration

## 1. 模块定位

集成测试。启动完整的 null capture → transport → client 链路，验证帧接收、协议头校验、时间戳一致性、序列号单调性，以及 UDP NACK/FEC 恢复能力。

## 2. 主要文件说明

| 文件 | 作用 |
|---|---|
| TransportIntegrationTest.cpp | TCP/UDP 集成测试。启动应用 → 连接客户端 → 接收 5 帧 → 校验协议头、payload、时间戳、序列号 → 验证 NACK/FEC 恢复 |
| RtpIntegrationTest.cpp | RTP 集成测试。启动应用 → 打开 RTP 接收 socket → 收集 RTP 帧 → 校验包序列、延迟扩展头、H.264 关键帧、SDP 文件 |

## 3. TransportIntegrationTest 测试逻辑

1. 加载配置（默认 `config/integration_tcp.conf`）
2. 校验 `embed_frame_metadata=true`
3. 启动 `AppBootstrap`
4. 等待 bound_port 就绪
5. 连接客户端（TCP 或 UDP）
6. 循环接收 5 帧：
   - 发送 KeepAlive（UDP 每 15 帧一次）
   - 接收帧（TCP 直接接收，UDP 分片重组）
   - 校验 MessageHeader magic
   - 校验 message_type == kAvStream
   - 校验 payload 非空且以 "null-frame-" 开头
   - 校验 capture_timestamp_ns 非零
   - 校验时间戳一致性（encode_start >= capture, encode_end >= encode_start）
   - 校验序列号单调递增
7. UDP 模式下验证 NACK/FEC 恢复被触发
8. 关闭连接，停止应用

返回值：
- 0：成功
- 1-14：各种失败场景（见源码注释）

## 4. RtpIntegrationTest 测试逻辑

1. 加载配置（默认 `config/integration_rtp_null.conf`）
2. 校验 `transport.backend=rtp`
3. V4L2 设备不可用时返回 77（CTest SKIP）
4. 打开 RTP 接收 socket（绑定到 rtp_remote_port）
5. 启动 `AppBootstrap`
6. 循环尝试收集完整的 RTP 帧（最多 8 次）：
   - `CollectFrame()`：接收 RTP 包直到 marker bit，重组为 Annex-B 数据
   - `ValidatePacketSequence()`：校验序列号连续、marker bit 位置正确
   - `ValidateLatencyExtension()`：校验延迟扩展头、时间戳一致性
   - 检查是否包含 SPS(7) + PPS(8) + IDR(5) NALU
7. 校验生成的 SDP 文件内容
8. 关闭 socket，停止应用

返回值：
- 0：成功
- 1-10：各种失败场景
- 77：V4L2 设备不可用（跳过）

## 5. CTest 配置

| 测试名 | 配置文件 | 标签 | 超时 |
|---|---|---|---|
| integration_transport_tcp | integration_tcp.conf | integration | 15s |
| integration_transport_udp | integration_udp.conf | integration | 15s |
| integration_transport_udp_fec | integration_udp_fec.conf | integration | 15s |
| integration_transport_udp_fec_nack | integration_udp_fec_nack.conf | integration;udp | 15s |
| integration_rtp_null | integration_rtp_null.conf | integration;rtp | 20s |
| integration_rtp_v4l2 | integration_rtp_v4l2.conf | integration;rtp;hardware | 25s |
