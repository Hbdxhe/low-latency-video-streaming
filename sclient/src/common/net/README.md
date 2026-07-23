# src/common/net

## 1. 模块定位

网络工具子模块。提供 H.264 流处理、RTP 协议解析和 SDP 文件解析等底层网络相关工具，被 `modules/network/` 使用。

## 2. 核心职责

- H.264 Annex B 流拼接（起始码 + NALU）
- H.264 IDR 帧检测
- RTP 包头解析和序列化
- RTP 头扩展解析（延迟扩展）
- SDP 文件解析（提取 RTP 地址、端口、payload type、clock rate）

## 3. 主要文件说明

| 文件 | 作用 |
|---|---|
| `H264AnnexB.h` | H.264 Annex B 工具：`AppendAnnexBNalu()`、`AppendAnnexBStartCode()`、`HasIdrNalUnit()` |
| `RtpProtocol.h` | RTP 协议工具：`ParseRtpHeader()`、`WriteRtpHeader()`、延迟扩展序列化/反序列化、字节序转换 |
| `SdpSessionDescription.h` | SDP 解析接口 |
| `SdpSessionDescription.cpp` | SDP 解析实现：提取 `c=IN IP4`、`m=video`、`a=rtpmap:` |

## 4. 核心类 / 函数说明

### H264AnnexB.h

作用：
- 被 `StreamClientRtp` 使用，将 RTP payload 重组为 Annex B 格式的 H.264 码流
- 被 `main.cpp` 使用，检测是否包含 IDR 帧（决定是否可以开始解码）

关键函数：
- `AppendAnnexBStartCode(output)`：追加 0x00000001 起始码
- `AppendAnnexBNalu(data, size, output)`：追加起始码 + NALU 数据
- `HasIdrNalUnit(data, size)`：扫描 Annex B 流，检测是否存在 IDR NALU（type=5）

### RtpProtocol.h

作用：
- 被 `StreamClientRtp` 使用，解析 RTP 包头和延迟扩展

关键函数：
- `ParseRtpHeader(data, size, fields, header_size, extension)`：解析 RTP 包头
- `WriteRtpHeader(fields, extension, packet)`：序列化 RTP 包头
- `ParseRtpLatencyExtension(extension, latency)`：解析延迟扩展（capture_timestamp_ns + transport_send_timestamp_ns）
- `BuildRtpLatencyHeaderExtension(latency)`：构建延迟扩展
- `ReadBe16/32/64()`、`WriteBe16/32/64()`：字节序转换

关键常量：
- `kRtpHeaderSize = 12`：RTP 基础头大小
- `kH264FuAType = 28`：H.264 FU-A 分片类型
- `kRtpLatencyExtensionProfileId = 0x5353`：延迟扩展的 profile ID

### SdpSessionDescription

作用：
- 被 `main.cpp` 使用，解析 SDP 文件获取 RTP 监听参数

关键结构体：
- `RtpVideoSessionDescription`：包含 `connection_address`、`video_port`、`payload_type`、`clock_rate`

关键函数：
- `ParseRtpVideoSessionDescription(contents, description, error)`：解析 SDP 内容
- `LoadRtpVideoSessionDescription(file_path, description, error)`：从文件加载 SDP

## 5. 数据流说明

- `H264AnnexB`：RTP payload → `AppendAnnexBNalu()` → Annex B 码流 → `HasIdrNalUnit()` 检测
- `RtpProtocol`：UDP 数据报 → `ParseRtpHeader()` → RTP 头字段 + 扩展
- `SdpSessionDescription`：SDP 文件 → `LoadRtpVideoSessionDescription()` → 监听地址/端口

## 6. 与其他模块的关系

- `H264AnnexB.h`：被 `modules/network/StreamClientRtp` 和 `app/main.cpp` 使用
- `RtpProtocol.h`：被 `modules/network/StreamClientRtp` 使用
- `SdpSessionDescription`：被 `app/main.cpp` 使用
- 不依赖其他模块（仅依赖标准库）

## 7. 线程模型 / 队列模型

本模块当前不直接管理线程。所有函数都是无状态的工具函数，线程安全。

## 8. 配置参数

无。本模块不涉及配置。

## 9. 调试建议

- RTP 解析失败：检查 `ParseRtpHeader` 返回值，确认 RTP 版本为 2
- SDP 解析失败：检查错误消息，确认 SDP 包含 `c=IN IP4`、`m=video`、`a=rtpmap:96 H264/90000`
- IDR 检测：`HasIdrNalUnit()` 扫描 Annex B 流中的 NALU type=5

## 10. 后续扩展方向

- 增加 H.265/HEVC 支持
- 增加 RTCP 解析
- 增加更多 RTP 头扩展支持
- 增加 SDP 生成能力（用于测试）
