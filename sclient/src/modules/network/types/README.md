# src/modules/network/types

## 1. 模块定位

网络模块的数据类型定义子目录。定义了网络接收模块使用的核心数据结构。

## 2. 核心职责

- 定义客户端配置结构体（`ClientConfig`）
- 定义接收到的帧数据结构（`ReceivedFrame`）
- 定义 UDP 接收统计结构体（`UdpReceiveStats`）

## 3. 主要文件说明

| 文件 | 作用 |
|---|---|
| `ClientConfig.h` | 客户端配置结构体，包含所有网络、jitter buffer、NACK/FEC、测试注入参数 |
| `ReceivedFrame.h` | 接收到的帧数据结构，包含 header、metadata、payload、时间戳 |
| `UdpReceiveStats.h` | UDP 接收统计，包含收包、丢包、抖动、jitter buffer、NACK/FEC 统计 |

## 4. 核心类 / 函数说明

### ClientConfig

作用：
- 客户端运行配置
- 由 CLI 解析填充，传给 `StreamClient::Connect()`

关键字段分组：
- 网络：`host`、`port`、`transport`、`sdp_path`
- UDP 基础：`udp_max_datagram_size`、`udp_receive_buffer_bytes`
- Jitter Buffer：`udp_jitter_buffer_enabled`、`udp_jitter_buffer_strategy`、`udp_jitter_buffer_target_delay_ms` 等
- NACK/FEC：`udp_nack_enabled`、`udp_fec_enabled`、`udp_nack_request_delay_ms` 等
- 测试注入：`udp_test_loss_pattern`、`udp_test_jitter_pattern` 等
- 保活：`keepalive_interval_ms`

### ReceivedFrame

作用：
- 网络模块输出的数据结构
- 从 `StreamClient::ReceiveFrame()` 输出，进入 `BoundedQueue`

关键字段：
- `header`：`MessageHeader`（消息头）
- `metadata`：`FrameDiagnosticMetadata`（延迟元数据）
- `sender_metadata_available`：发送端是否提供了有效的延迟元数据
- `receive_timestamp_ns`：本地接收时间戳（`CLOCK_MONOTONIC` 纳秒）
- `payload`：帧载荷（H.264 Annex B 码流）

### UdpReceiveStats

作用：
- UDP 接收统计的完整快照
- 由 `StreamClient` 维护，通过 `udp_receive_stats()` 获取

关键字段分组：
- 收包：`datagrams_received`、`fragments_received`、`completed_frames`
- 丢包：`invalid_datagrams`、`timed_out_fragments`、`timed_out_frames`、`duplicate_fragments`
- 抖动：`jitter_last_ms`、`jitter_avg_ms`、`jitter_max_ms`、`jitter_p50_ms`、`jitter_p95_ms`
- Jitter Buffer：`jitter_buffer_current_depth`、`jitter_buffer_max_depth`、`jitter_buffer_target_delay_ms`、`jitter_buffer_emitted_frames`、`jitter_buffer_skipped_frames`、`jitter_buffer_dropped_frames`
- NACK/FEC：`nack_requests_sent`、`fec_recovered_fragments`、`fec_recovered_frames`
- 测试注入：`injected_loss_datagrams`、`injected_loss_fragments`

## 5. 数据流说明

- `ClientConfig`：CLI → `StreamClient::Connect()`
- `ReceivedFrame`：`StreamClient::ReceiveFrame()` → `BoundedQueue` → `VideoDecoder`
- `UdpReceiveStats`：`StreamClient` 内部更新 → `main.cpp` 读取 → HUD 展示

## 6. 与其他模块的关系

- 被 `modules/network/StreamClient` 使用
- 被 `app/main.cpp` 使用
- 被 `app/cli/CliOptions` 使用（填充 ClientConfig）

## 7. 线程模型 / 队列模型

本模块当前不直接管理线程。所有结构体是纯 POD 类型。

## 8. 配置参数

本模块定义了所有配置参数的结构。具体参数说明见各结构体字段注释和 `ClientConfig.h`。

## 9. 调试建议

- 检查 `ReceivedFrame.payload` 是否为空
- 检查 `sender_metadata_available` 确认延迟元数据是否可用
- 检查 `UdpReceiveStats` 中的各项统计判断网络状况

## 10. 后续扩展方向

- 增加更多统计字段
- 增加统计导出接口（JSON/Protobuf）
