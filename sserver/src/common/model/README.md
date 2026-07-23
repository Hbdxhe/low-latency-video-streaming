# common/model

## 1. 模块定位

公共数据模型。定义 capture → transport 之间传递的核心数据结构 `EncodedFrame`。

## 2. 核心职责

- 定义 `EncodedFrame` 结构体，承载编码后的帧数据和元信息
- 定义 `StreamPayloadType` 枚举，区分视频/音频流类型
- 定义 `EncodedFramePtr` 智能指针类型别名

## 3. 主要文件说明

| 文件 | 作用 |
|---|---|
| EncodedFrame.h | 定义 `EncodedFrame`、`StreamPayloadType`、`EncodedFramePtr` |

## 4. 核心类 / 函数说明

### EncodedFrame

作用：
- 作为整个数据链路中的核心数据载体
- 从采集模块产生，经过编码模块填充，最终传递给传输模块

字段：
- `type`：流类型（`kVideo` = 3，`kAudio` = 4）
- `sequence`：帧序列号，用于跟踪和诊断
- `capture_timestamp_ns`：采集时间戳（单调时钟纳秒）
- `encode_start_timestamp_ns`：编码开始时间戳
- `encode_end_timestamp_ns`：编码结束时间戳
- `is_keyframe`：是否为关键帧
- `payload`：编码后的数据（H.264 Annex-B 格式）

### StreamPayloadType

- `kVideo = 3`：视频流
- `kAudio = 4`：音频流（当前未使用）

## 5. 数据流说明

输入：
- 采集模块填充 `capture_timestamp_ns`
- 编码模块填充 `encode_start/end_timestamp_ns`、`is_keyframe`、`payload`

输出：
- 传递给传输模块的 `Broadcast()` 方法
- 传输模块读取所有字段用于打包和延迟计算

## 6. 与其他模块的关系

- 被 `CaptureModule` 产生
- 被 `NullCaptureDevice` / `V4L2CaptureDevice` 填充
- 被 `TransportModule` / `Transport` / 各 `ITransportBackend` 消费
- 被 `RtpPacketizer` 用于 RTP 打包
- 被 `TcpClientSession` 用于 TCP 发送
