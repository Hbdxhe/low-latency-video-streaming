# src/common/protocol

## 1. 模块定位

应用层协议定义子模块。定义了 sclient 与 sserver 之间通信的所有数据格式，包括消息头、UDP 分片、NACK 请求和接收报告。

## 2. 核心职责

- 定义消息头格式（`MessageHeader`）
- 定义帧诊断元数据（`FrameDiagnosticMetadata`）
- 定义 UDP 分片头（`UdpFrameFragmentHeader`）
- 定义 NACK 请求格式（`UdpNackHeader`、`UdpNackItem`）
- 定义 UDP 接收报告（`UdpReceiverReport`）
- 提供消息魔数校验

## 3. 主要文件说明

| 文件 | 作用 |
|---|---|
| `Protocol.h` | 所有协议结构体定义，使用 `#pragma pack(push, 1)` 保证内存布局与线路格式一致 |

## 4. 核心类 / 函数说明

### MessageHeader

作用：
- 所有消息的统一头部
- 12 字节：魔数(4) + 消息类型(2) + 子类型(2) + 载荷长度(4)

关键字段：
- `head_id[4]`：魔数，固定为 `CCTC`
- `message_type`：消息类型（`kKeepAlive=0`、`kAvStream=1`、`kUdpNack=2`）
- `sub_type`：子类型（当前未使用）
- `payload_length`：载荷长度

### FrameDiagnosticMetadata

作用：
- 帧级诊断元数据，由发送端附加在每帧数据前面
- 用于计算端到端延迟

关键字段：
- `sequence`：帧序号
- `capture_timestamp_ns`：采集时间戳（纳秒）
- `encode_start_timestamp_ns`：编码开始时间戳
- `encode_end_timestamp_ns`：编码结束时间戳
- `transport_send_timestamp_ns`：传输发送时间戳

### UdpFrameFragmentHeader

作用：
- UDP 分片头，标识一个分片属于哪一帧的哪个位置
- 支持数据分片和 FEC 奇偶校验分片

关键字段：
- `frame_sequence`：帧序号
- `frame_payload_size`：整帧载荷大小
- `fragment_offset`：本分片在帧内的偏移
- `fragment_index`：分片索引
- `fragment_count`：总分片数
- `fragment_role`：分片角色（`kData=0` 或 `kXorParity=1`）
- `capture_timestamp_ns` / `transport_send_timestamp_ns`：延迟元数据

### UdpNackHeader / UdpNackItem

作用：
- NACK 请求格式
- 客户端检测到缺失分片后，发送 NACK 请求服务端重传

### UdpReceiverReport

作用：
- UDP 接收报告，客户端定期发送给服务端
- 包含接收统计和抖动信息

### MessageType 枚举

- `kKeepAlive = 0`：心跳保活
- `kAvStream = 1`：音视频流数据
- `kUdpNack = 2`：UDP NACK 请求

### UdpFragmentRole 枚举

- `kData = 0`：数据分片
- `kXorParity = 1`：XOR 奇偶校验分片

### 工具函数

- `FillMessageMagic(buffer)`：填充魔数
- `HasValidMessageMagic(header)`：校验魔数

## 5. 数据流说明

发送方向（服务端→客户端）：
```
MessageHeader + UdpFrameFragmentHeader + 分片数据 → UDP 数据报
MessageHeader + FrameDiagnosticMetadata + 帧数据 → TCP 消息
```

接收方向（客户端→服务端）：
```
MessageHeader + UdpReceiverReport → UDP 心跳
MessageHeader + UdpNackHeader + UdpNackItem[] → UDP NACK
```

## 6. 与其他模块的关系

- 被 `modules/network/StreamClient` 使用：解析收到的消息
- 被 `app/main.cpp` 使用：访问 `FrameDiagnosticMetadata` 计算延迟
- 不依赖其他模块

## 7. 线程模型 / 队列模型

本模块当前不直接管理线程。所有结构体是纯 POD 类型，使用 `#pragma pack(push, 1)` 保证二进制布局。

## 8. 配置参数

无。协议格式是硬编码的。

## 9. 调试建议

- 检查魔数：`HasValidMessageMagic()` 返回 false 说明数据损坏或协议不匹配
- 检查消息类型：`message_type` 应为 0/1/2
- 检查分片完整性：`fragment_index` 应 < `fragment_count`
- 使用 Wireshark 抓包时，UDP 载荷前 12 字节是 `MessageHeader`，紧接着是 `UdpFrameFragmentHeader`

## 10. 后续扩展方向

- 增加协议版本号
- 增加消息校验和
- 增加更多消息类型（控制面、信令）
- 增加协议文档自动生成
