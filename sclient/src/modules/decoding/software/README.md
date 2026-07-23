# src/modules/decoding/software

## 1. 模块定位

软件解码后端实现。使用 FFmpeg `libavcodec` 进行 H.264 软件解码，是当前唯一的解码后端。

## 2. 核心职责

- 使用 FFmpeg 查找并初始化 H.264 解码器
- 接收 H.264 Annex B 码流，输出解码后的 YUV 帧
- 管理 AVFrame 的生命周期（通过 shared_ptr）

## 3. 主要文件说明

| 文件 | 作用 |
|---|---|
| `SoftwareVideoDecoderBackend.cpp` | FFmpeg 软件解码实现，包含 `SoftwareVideoDecoderBackend` 类和工厂函数 |

## 4. 核心类 / 函数说明

### SoftwareVideoDecoderBackend

作用：
- `VideoDecoderBackend` 的具体实现
- 使用 FFmpeg `avcodec` API 进行 H.264 解码

关键函数：
- `Initialize()`：`avcodec_find_decoder(AV_CODEC_ID_H264)` → `avcodec_alloc_context3` → `avcodec_open2` → `av_frame_alloc`
- `Decode()`：构造 `AVPacket` → `avcodec_send_packet` → 循环 `avcodec_receive_frame` → `av_frame_clone` → 构造 `DecodedFrame`
- `Shutdown()`：`av_frame_free` → `avcodec_free_context`

### 辅助函数

- `ResolveDecodedPixelFormat()`：将 AVPixelFormat 映射为 DecodedPixelFormat
- `AvErrorString()`：FFmpeg 错误码转字符串
- `CopyDecodedFrameView()`：从 AVFrame 拷贝数据指针到 DecodedFrame
- `MakeAvFrameOwner()`：创建 shared_ptr<void> 管理 AVFrame 生命周期

## 5. 数据流说明

输入：
- `const uint8_t *data` + `size_t size`：H.264 Annex B 码流

处理：
- `avcodec_send_packet()`：送入解码器
- `avcodec_receive_frame()`：取出解码帧
- `av_frame_clone()`：克隆帧（FFmpeg 内部会复用）
- `MakeAvFrameOwner()`：创建 RAII 包装

输出：
- `DecodedFrame`：width/height/pixel_format/data[]/linesize[]/owner

## 6. 与其他模块的关系

- 实现 `VideoDecoderBackend` 接口
- 被 `VideoDecoder` 通过工厂函数 `CreateSoftwareVideoDecoderBackend()` 创建
- 使用 `common/media/DecodedFrame.h` 定义输出结构
- 依赖 `libavcodec`、`libavutil`

## 7. 线程模型 / 队列模型

本模块当前不直接管理线程。在解码线程中被调用，内部无线程。

## 8. 配置参数

无额外配置。解码器使用 FFmpeg 默认参数。

## 9. 调试建议

- **找不到解码器**：`failed to find H.264 decoder` — 检查 FFmpeg 安装
- **解码失败**：`avcodec_send_packet failed` — 检查码流格式是否正确
- **无输出帧**：`decoder did not output a frame` — 可能需要等待更多数据或关键帧
- 适合打断点：`avcodec_receive_frame()` 返回值检查处

## 10. 后续扩展方向

- 增加硬件解码后端（VAAPI、NVDEC）
- 增加解码线程数配置
- 增加解码错误恢复
