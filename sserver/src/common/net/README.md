# common/net

## 1. 模块定位

网络协议结构与工具。定义 TCP/UDP 自定义协议的线上格式、RTP 协议头解析/序列化、H.264 Annex-B NALU 分割工具。

## 2. 核心职责

- 定义 TCP/UDP 通用的消息头格式（`MessageHeader`）
- 定义 UDP 分片头格式（`UdpFrameFragmentHeader`）
- 定义 UDP 客户端报告格式（`UdpReceiverReport`）
- 定义 UDP NACK 请求格式（`UdpNackHeader`、`UdpNackItem`）
- 定义帧诊断元数据格式（`FrameDiagnosticMetadata`）
- 提供 RTP 协议头的读写和解析（`RtpHeaderFields`、`WriteRtpHeader`、`ParseRtpHeader`）
- 提供 RTP 延迟扩展头的序列化和解析（`RtpLatencyExtension`）
- 提供 H.264 Annex-B start code 查找和 NALU 分割工具

## 3. 主要文件说明

| 文件 | 作用 |
|---|---|
| StreamProtocol.h | 定义 TCP/UDP 自定义协议的线上格式：`MessageHeader`（magic + type + length）、`UdpFrameFragmentHeader`（分片信息）、`UdpReceiverReport`（客户端统计）、`UdpNackHeader`/`UdpNackItem`（NACK 请求）、`FrameDiagnosticMetadata` |
| RtpProtocol.h | RTP 协议工具：`RtpHeaderFields` 结构体、`WriteRtpHeader()`/`ParseRtpHeader()` 函数、`RtpLatencyExtension` 延迟扩展头、Big-Endian 读写工具函数 |
| H264AnnexB.h | H.264 Annex-B 工具：`H264NaluView` 结构体、`FindAnnexBStartCode()` 查找 start code、`SplitAnnexBNalus()` 分割 NALU、`AppendAnnexBNalu()` 拼接 NALU |

## 4. 核心类 / 函数说明

### StreamProtocol.h

#### MessageHeader
```cpp
struct MessageHeader {
    char head_id[4];          // 魔数 "CCTC"
    uint16_t message_type;    // 0=KeepAlive, 1=AvStream, 2=UdpNack
    uint16_t sub_type;        // 子类型（如 StreamPayloadType）
    uint32_t payload_length;  // payload 长度
};
```

#### UdpFrameFragmentHeader
```cpp
struct UdpFrameFragmentHeader {
    uint64_t frame_sequence;           // 帧序列号
    uint64_t capture_timestamp_ns;     // 采集时间戳
    uint64_t encode_start_timestamp_ns;
    uint64_t encode_end_timestamp_ns;
    uint64_t transport_send_timestamp_ns;
    uint32_t frame_payload_size;       // 整帧 payload 大小
    uint32_t fragment_offset;          // 分片偏移
    uint16_t fragment_index;           // 分片索引
    uint16_t fragment_count;           // 总分片数
    uint16_t fragment_role;            // 0=Data, 1=XorParity
    uint16_t reserved;
};
```

### RtpProtocol.h

#### 关键常量
- `kRtpHeaderSize = 12`：RTP 固定头大小
- `kRtpLatencyExtensionProfileId = 0x5353`：自定义延迟扩展头 profile ID
- `kRtpPacketOverheadWithLatencyExtension = 32`：带延迟扩展头的 RTP 包总开销

#### 关键函数
- `WriteRtpHeader(fields, extension, packet)`：序列化 RTP 包头
- `ParseRtpHeader(data, size, fields, header_size, extension)`：解析 RTP 包头
- `BuildRtpLatencyHeaderExtension(extension)`：构建延迟扩展头
- `UpdateRtpLatencyExtensionTransportSendTimestamp(data, size, ts)`：就地更新发送时间戳（避免重新序列化）

### H264AnnexB.h

#### 关键函数
- `FindAnnexBStartCode(data, offset, prefix_size)`：从 offset 开始查找 Annex-B start code（0x000001 或 0x00000001）
- `SplitAnnexBNalus(data, output)`：将 Annex-B 数据分割为多个 `H264NaluView`
- `AppendAnnexBNalu(data, size, output)`：向 output 追加一个带 start code 的 NALU

#### H264NaluView
- `data`：NALU 数据指针（不含 start code）
- `size`：NALU 数据大小
- `nal_type()`：返回 NAL 类型（`data[0] & 0x1F`）

## 5. 与其他模块的关系

- `StreamProtocol` 被 `TcpClientSession`、`TcpStreamingBackend`、`UdpStreamingBackend` 使用
- `RtpProtocol` 被 `RtpPacketizer`、`RtpStreamingBackend` 使用
- `H264AnnexB` 被 `RtpPacketizer` 使用，用于将 H.264 Annex-B 数据分割为 NALU 后进行 RTP 打包
- `MessageHeader` 中的 `kMessageMagic`（"CCTC"）用于协议识别

## 6. 注意事项

- 所有协议结构体使用 `#pragma pack(push, 1)` 确保字节对齐，直接用于网络传输
- RTP 延迟扩展头使用自定义 profile ID `0x5353`，非标准 RTP 扩展
- H.264 Annex-B start code 查找支持 3 字节（0x000001）和 4 字节（0x00000001）两种格式
