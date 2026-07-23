# src/modules

## 1. 模块定位

业务模块层。包含客户端管线的三大核心模块：网络接收、视频解码、视频渲染。每个模块都采用"统一接口 + 后端工厂"的架构，方便扩展新的传输协议、解码后端和渲染后端。

## 2. 核心职责

- `network/`：通过 TCP/UDP/RTP 接收视频流，处理分片重组、NACK、FEC、抖动缓冲
- `decoding/`：将 H.264 码流解码为 YUV 像素数据
- `rendering/`：将解码后的帧渲染到屏幕，叠加 HUD 信息

## 3. 子目录说明

| 子目录 | 作用 |
|---|---|
| `network/` | 网络接收模块，支持 TCP/UDP/RTP 三种传输协议 |
| `decoding/` | 视频解码模块，当前支持 FFmpeg 软件解码 |
| `rendering/` | 视频渲染模块，当前支持 OpenGL 渲染后端 |

## 4. 架构模式

所有模块采用相同的架构模式：

```
XxxBackend.h          # 抽象基类，定义接口
Xxx.h / Xxx.cpp       # 统一接口 + 工厂
<impl>/               # 具体后端实现
```

- `VideoDecoderBackend` → `SoftwareVideoDecoderBackend`
- `VideoRendererBackend` → `OpenGlVideoRendererBackend`

这种设计使得新增后端（如硬件解码、Vulkan 渲染）只需实现对应的 Backend 接口。

## 5. 数据流

```
modules/network → ReceivedFrame → modules/decoding → DecodedFrame → modules/rendering → 屏幕
```

## 6. 与其他模块的关系

- 三个模块都依赖 `common/` 提供的基础能力
- 由 `app/main.cpp` 编排串联
- 模块之间通过 `BoundedQueue` 解耦，不直接调用

## 7. 线程模型

- `network/`：在接收线程中运行
- `decoding/`：在解码线程中运行
- `rendering/`：在主线程（渲染线程）中运行
- 模块之间通过有界队列传递数据
