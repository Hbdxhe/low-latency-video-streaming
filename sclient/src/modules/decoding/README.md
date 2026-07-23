# src/modules/decoding

## 1. 模块定位

视频解码模块。负责将 H.264 Annex B 码流解码为 YUV 像素数据，输出 `DecodedFrame` 给渲染模块。采用"统一接口 + 后端工厂"架构，当前仅实现 FFmpeg 软件解码。

## 2. 核心职责

- 接收 H.264 Annex B 码流（来自网络模块的 `ReceivedFrame.payload`）
- 使用 FFmpeg `avcodec` 进行 H.264 软件解码
- 输出 `DecodedFrame`（含 YUV420P/NV12 数据指针和生命周期管理）
- 支持解码后端扩展（当前仅 software）

## 3. 主要文件说明

| 文件 | 作用 |
|---|---|
| `VideoDecoder.h` | 解码器统一接口。定义 `Initialize()`、`Decode()`、`Shutdown()` |
| `VideoDecoder.cpp` | 解码器工厂：根据 `DecodeBackend` 创建对应后端 |
| `VideoDecoderBackend.h` | 解码后端抽象基类。定义 `Initialize()`、`Decode()`、`Shutdown()`、`backend_name()` |
| `software/SoftwareVideoDecoderBackend.cpp` | FFmpeg 软件解码实现 |

## 4. 核心类 / 函数说明

### VideoDecoder

作用：
- 解码器统一接口
- 被 `main.cpp` 的解码线程调用
- 内部持有 `VideoDecoderBackend` 的具体实现

关键函数：
- `Initialize(backend, error)`：初始化解码器，创建并初始化后端
- `Decode(data, size, decoded_frame, error)`：解码一帧，委托给后端
- `Shutdown()`：释放解码资源

### VideoDecoderBackend

作用：
- 解码后端抽象基类
- 定义了解码后端必须实现的接口

关键函数：
- `Initialize(error)`：初始化后端
- `Decode(data, size, decoded_frame, error)`：解码一帧
- `Shutdown()`：释放资源
- `backend_name()`：返回后端名称

### SoftwareVideoDecoderBackend

作用：
- FFmpeg 软件解码实现
- 使用 `avcodec_find_decoder(AV_CODEC_ID_H264)` 查找解码器
- 使用 `avcodec_send_packet()` / `avcodec_receive_frame()` 解码

关键流程：
1. `Initialize()`：查找 H.264 解码器 → 分配上下文 → 打开解码器 → 分配帧
2. `Decode()`：构造 `AVPacket` → `avcodec_send_packet()` → 循环 `avcodec_receive_frame()` → 克隆帧 → 构造 `DecodedFrame`
3. `Shutdown()`：释放帧 → 释放上下文

关键设计：
- `DecodedFrame.owner` 使用 `shared_ptr<void>` 管理 `AVFrame*` 的生命周期
- `DecodedFrame.data[]` 直接指向 AVFrame 内部缓冲区（零拷贝）
- 支持 YUV420P、YUVJ420P（映射为 kYuv420p）和 NV12 像素格式

### DecodeBackend 枚举

- `kAuto`：自动选择（当前等同于 kSoftware）
- `kSoftware`：FFmpeg 软件解码

## 5. 数据流说明

输入：
- `ReceivedFrame.payload`：H.264 Annex B 码流（来自网络模块）

处理：
- `avcodec_send_packet()`：送入 FFmpeg 解码器
- `avcodec_receive_frame()`：取出解码后的 AVFrame
- `av_frame_clone()`：克隆帧（因为 FFmpeg 内部会复用）
- `MakeAvFrameOwner()`：创建 shared_ptr 管理生命周期
- `CopyDecodedFrameView()`：拷贝数据指针和 linesize

输出：
- `DecodedFrame`：包含 width/height/pixel_format/data[]/linesize[]/owner

## 6. 与其他模块的关系

```
modules/decoding/
  ├── 输入：ReceivedFrame.payload（来自 modules/network）
  ├── 输出：DecodedFrame（给 modules/rendering）
  ├── 使用 common/media/DecodedFrame.h 定义输出结构
  ├── 使用 common/log/Logger 输出日志
  └── 依赖外部库：libavcodec、libavutil
```

## 7. 线程模型 / 队列模型

本模块当前不直接管理线程。`VideoDecoder` 在解码线程中被调用（由 `main.cpp` 创建），内部无线程。

线程安全：
- `VideoDecoder` 实例仅在解码线程中访问，无需额外同步
- `DecodedFrame.owner`（shared_ptr）在跨线程传递时自动管理引用计数

## 8. 配置参数

| 参数 | 说明 |
|---|---|
| `--decoder` | 解码后端选择：`auto`（默认）或 `software` |

当前无码率、GOP、线程数等编码参数配置（因为是解码端）。

## 9. 调试建议

- **解码失败**：查看日志中 `avcodec_send_packet failed` 或 `avcodec_receive_frame failed` 的错误信息
- **无输出帧**：`decoder did not output a frame` 通常是因为缺少 SPS/PPS，需要等待关键帧
- **像素格式不支持**：检查 `DecodedPixelFormat` 是否为 `kYuv420p` 或 `kNv12`
- **适合打断点的位置**：
  - `SoftwareVideoDecoderBackend::Decode()` 中 `avcodec_send_packet()` 之后
  - `avcodec_receive_frame()` 返回值检查
  - `CopyDecodedFrameView()` 确认帧数据正确

## 10. 后续扩展方向

- 增加硬件解码后端（VAAPI、NVDEC、VideoToolbox）
- 增加 H.265/HEVC 解码支持
- 增加解码线程数配置
- 增加解码性能统计（解码耗时、GPU 利用率）
- 增加解码错误恢复策略
