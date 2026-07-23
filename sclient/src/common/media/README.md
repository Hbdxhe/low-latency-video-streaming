# src/common/media

## 1. 模块定位

媒体数据结构子模块。定义解码后帧的数据结构 `DecodedFrame`，是解码模块和渲染模块之间的数据桥梁。

## 2. 核心职责

- 定义 `DecodedFrame` 结构体：描述解码后的视频帧
- 定义 `DecodedPixelFormat` 枚举：支持 YUV420P 和 NV12 像素格式
- 通过 `std::shared_ptr<void> owner` 管理解码帧的生命周期（AVFrame 的 RAII 封装）

## 3. 主要文件说明

| 文件 | 作用 |
|---|---|
| `DecodedFrame.h` | 定义 `DecodedFrame` 和 `DecodedPixelFormat` |

## 4. 核心类 / 函数说明

### DecodedFrame

作用：
- 解码后帧的数据载体
- 从 `VideoDecoder` 输出，传入 `VideoRenderer`
- 使用零拷贝设计：`data` 指针直接指向 AVFrame 内部缓冲区，`owner` 管理生命周期

关键字段：
- `width` / `height`：帧尺寸
- `pixel_format`：像素格式（`kYuv420p` 或 `kNv12`）
- `data[4]`：各平面数据指针（Y/U/V 或 Y/UV）
- `linesize[4]`：各平面行字节数（可能含 padding）
- `owner`：`shared_ptr<void>`，实际管理 `AVFrame*` 的释放
- `empty()`：判断帧是否有效

### DecodedPixelFormat

- `kUnknown`：未知格式
- `kYuv420p`：YUV 4:2:0 平面格式（3 个平面）
- `kNv12`：NV12 格式（Y 平面 + 交织 UV 平面）

## 5. 数据流说明

输入：
- `SoftwareVideoDecoderBackend::Decode()` 填充 `DecodedFrame`

输出：
- `OpenGlVideoRendererBackend::Render()` 消费 `DecodedFrame`

## 6. 与其他模块的关系

- 被 `modules/decoding/` 输出
- 被 `modules/rendering/` 输入
- 不依赖其他模块

## 7. 线程模型 / 队列模型

本模块当前不直接管理线程。`DecodedFrame` 是纯数据结构，线程安全性由使用方（`BoundedQueue`）保证。

## 8. 配置参数

无。本模块不涉及配置。

## 9. 调试建议

- 如果渲染报 "decoded frame format is not supported"，检查 `pixel_format` 是否为 `kYuv420p` 或 `kNv12`
- 如果出现花屏，检查 `linesize` 是否正确（可能含对齐 padding）

## 10. 后续扩展方向

- 增加更多像素格式支持（P010、BGR 等）
- 增加帧的时间戳字段
- 增加帧的引用计数机制（替代 shared_ptr）
