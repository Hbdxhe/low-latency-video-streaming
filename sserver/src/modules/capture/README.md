# modules/capture

## 1. 模块定位

视频采集模块。负责从摄像头设备（V4L2）或模拟源（Null）采集视频帧，调用编码器将原始帧编码为 H.264，输出 `EncodedFrame` 给传输模块。

## 2. 核心职责

- 提供 `ICaptureDevice` 接口，抽象采集设备能力
- 提供 `Capture` 门面类，封装设备创建和生命周期管理
- 提供 `CaptureModule`（`IModule` 实现），管理采集线程和编码线程
- 实现 `NullCaptureDevice`：模拟采集设备，用于测试
- 实现 `V4L2CaptureDevice`：Linux V4L2 摄像头采集设备
- 支持单线程模式（采集+编码同线程）和双线程模式（采集线程+编码线程分离）

## 3. 主要文件说明

| 文件 | 作用 |
|---|---|
| video/ICaptureDevice.h | 定义 `ICaptureDevice` 接口和 `RawCaptureFrame` 结构体 |
| video/Capture.h / .cpp | `Capture` 门面类，封装设备创建（`CaptureBackendFactory`）和生命周期管理 |
| video/CaptureModule.h / .cpp | `CaptureModule`（`IModule` 实现），管理采集/编码线程，通过 `FrameHandler` 回调输出编码帧 |
| video/null/NullCaptureDevice.h / .cpp | 模拟采集设备，支持 text 模式（纯文本 payload）和 h264_test_pattern 模式（合成 YUYV 帧 → x264 编码） |
| video/v4l2/V4L2CaptureDevice.h / .cpp | V4L2 摄像头采集设备，支持 mmap 内核缓冲区、YUYV422 格式、硬件时间戳 |

## 4. 核心类 / 函数说明

### ICaptureDevice

作用：
- 采集设备的抽象接口
- 支持两种采集模式：直接编码输出（`CaptureFrame`）和原始帧采集+分离编码（`CaptureRawFrame` + `EncodeRawFrame`）

关键函数：
- `Open()` / `Start()` / `Stop()` / `Close()`：设备生命周期
- `CaptureFrame()`：采集一帧并直接编码输出 `EncodedFrame`
- `SupportsRawCapture()`：是否支持原始帧采集模式
- `CaptureRawFrame()`：采集原始帧（不编码）
- `EncodeRawFrame(raw)`：将原始帧编码为 `EncodedFrame`
- `Describe()`：返回设备描述字符串

### CaptureModule

作用：
- 实现 `IModule` 接口，作为采集模块的生命周期管理入口
- 根据设备是否支持原始帧采集，选择单线程或双线程模式

关键函数：
- `initialize(context)`：初始化 `Capture` 门面
- `start()`：启动采集。如果设备支持原始帧采集（V4L2），启动 `CapturePump` + `EncodePump` 双线程；否则启动 `CaptureLoop` 单线程
- `stop()`：停止采集线程，等待线程退出
- `SetFrameHandler(handler)`：设置帧输出回调（由 `AppBootstrap` 绑定到 `TransportModule::Broadcast`）

### Capture

作用：
- 门面类，封装 `ICaptureDevice` 的创建和生命周期
- 通过 `CaptureBackendFactory` 根据配置创建具体的设备实现

关键函数：
- `initialize(context, error_message)`：解析采集后端配置，创建设备实例
- `start(error_message)`：调用 `device->Open()` + `device->Start()`
- `CaptureFrame()` / `CaptureRawFrame()` / `EncodeRawFrame()`：代理到设备实现

### V4L2CaptureDevice

作用：
- Linux V4L2 摄像头采集设备实现
- 使用 mmap 内核缓冲区实现零拷贝采集
- 支持 YUYV422 像素格式
- 支持硬件单调时间戳（如果驱动提供）
- 支持原始帧采集模式（双线程架构下使用）

关键函数：
- `Open()`：打开 `/dev/videoX` 设备文件
- `Start()`：查询设备能力 → 配置格式 → 配置帧率 → 初始化 mmap 缓冲区 → 初始化编码器 → 开始流
- `CaptureRawFrame()`：`VIDIOC_DQBUF` → 拷贝帧数据 → `VIDIOC_QBUF`
- `EncodeRawFrame(raw)`：调用 `VideoEncoder::EncodeYuyv422Frame()` 编码
- `CaptureFrame()`：`VIDIOC_DQBUF` → 编码 → `VIDIOC_QBUF`（单线程模式）

### NullCaptureDevice

作用：
- 模拟采集设备，用于无摄像头环境下的测试
- 两种模式：`text`（生成纯文本 payload）和 `h264_test_pattern`（合成 YUYV 帧 → x264 编码）

## 5. 数据流说明

### 单线程模式（NullCaptureDevice）

```
CaptureLoop 线程:
  NullCaptureDevice::CaptureFrame()
  → 生成合成帧 → 编码（如果是 h264_test_pattern 模式）
  → EncodedFrame
  → FrameHandler 回调 → TransportModule::Broadcast()
```

### 双线程模式（V4L2CaptureDevice）

```
CapturePump 线程:                    EncodePump 线程:
  V4L2CaptureDevice::CaptureRawFrame()  ThreadSafeQueue::WaitPopFor()
  → VIDIOC_DQBUF → 拷贝 → VIDIOC_QBUF  → V4L2CaptureDevice::EncodeRawFrame()
  → RawCaptureFrame                     → ConvertYuyv422ToI420 (NEON/scalar)
  → ThreadSafeQueue::PushDropOldest()   → x264_encoder_encode
                                         → EncodedFrame
                                         → FrameHandler 回调 → TransportModule::Broadcast()
```

## 6. 与其他模块的关系

- 被 `AppBootstrap` 创建和注册为 `IModule`
- 通过 `FrameHandler` 回调将 `EncodedFrame` 传递给 `TransportModule::Broadcast()`
- 使用 `encoding/VideoEncoder`（内部使用 `X264VideoEncoderBackend`）进行 H.264 编码
- 使用 `common/model/EncodedFrame.h` 定义输出数据结构
- 使用 `common/concurrency/ThreadSafeQueue` 进行跨线程帧传递
- 使用 `common/time/MonotonicClock` 获取时间戳
- 使用 `common/log/Logger` 输出日志
- 从 `config/AppConfig` 读取 `CaptureConfig` 和 `CodecConfig`

## 7. 线程模型 / 队列模型

### 双线程架构（V4L2 设备）

```
CapturePump 线程                EncodePump 线程
     │                              │
     │  CaptureRawFrame()           │  WaitPopFor()
     │         │                    │         │
     │  PushDropOldest(raw_queue_)  │  EncodeRawFrame()
     │         │                    │         │
     └────────►│────────────────────►  FrameHandler(frame)
               raw_queue_                    │
               (深度3,满丢旧)          TransportModule::Broadcast()
```

- `raw_queue_`：`ThreadSafeQueue<RawCaptureFramePtr>`，最大深度 3，满时丢弃最旧帧
- `running_`：`atomic_bool`，用于通知线程退出
- `handler_mutex_`：保护 `frame_handler_` 的读写

### 单线程架构（Null 设备）

单个 `worker_thread_` 串行执行采集和编码。

## 8. 配置参数

| 参数 | 说明 | 默认值 |
|---|---|---|
| capture.enabled | 是否启用采集 | true |
| capture.source | 采集源类型 | "v4l2" |
| capture.device | V4L2 设备路径 | "/dev/video0" |
| capture.width | 采集宽度 | 640 |
| capture.height | 采集高度 | 360 |
| capture.fps | 采集帧率 | 30 |
| capture.frame_interval_ms | Null 设备帧间隔 | 0 |
| capture.device_buffer_count | V4L2 内核缓冲区数 | 2 |
| capture.null_payload_bytes | Null 设备 payload 填充大小 | 0 |
| capture.null_payload_mode | Null 设备模式 | "text" |

## 9. 调试建议

- 检查日志中 "capture module started with ..." 行，确认设备描述和线程模式
- V4L2 设备：检查是否成功打开 `/dev/videoX`，日志中是否有 "failed to open capture device"
- V4L2 设备：检查是否使用硬件时间戳（"v4l2 capture is using hardware monotonic buffer timestamps"）
- 编码失败：检查 "video encoder dropped frame" 日志
- 双线程模式下如果丢帧严重，检查 "raw_queue_" 深度（当前代码中未直接输出，可通过编码帧序列号间隙推断）
- 使用 `capture.source=null` 和 `capture.null_payload_mode=h264_test_pattern` 可在无摄像头环境下测试完整链路

## 10. 后续扩展方向

- 增加更多采集源：RTSP、文件、屏幕捕获
- 支持更多像素格式（NV12、I420 等）
- 增加采集帧率自适应
- 增加采集分辨率动态切换
- 将编码从采集模块中解耦为独立的 `EncodingModule`
- 增加音频采集支持（`capture/audio/`）
