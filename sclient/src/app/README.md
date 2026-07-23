# src/app

## 1. 模块定位

应用入口层。负责程序启动、命令行参数解析、管线（pipeline）编排和生命周期管理。这是整个客户端的"总指挥"，把网络接收、解码、渲染三个模块串起来。

## 2. 核心职责

- 解析命令行参数（`CliOptions`）
- 初始化网络连接（`StreamClient`）、解码器（`VideoDecoder`）、渲染器（`VideoRenderer`）
- 启动接收线程和解码线程，主线程负责渲染
- 通过 `BoundedQueue` 在线程之间传递帧数据
- 统计各阶段延迟（`LatencyStats`）并输出日志
- 处理信号（SIGINT/SIGTERM）实现优雅退出

## 3. 主要文件说明

| 文件 | 作用 |
|---|---|
| `main.cpp` | 程序入口。创建三线程管线（接收→解码→渲染），管理生命周期，统计延迟，处理键盘输入 |
| `cli/CliOptions.h` | CLI 解析接口声明。定义 `ParseClientOption()`、`ParseUdpBenchmarkOption()` 等函数 |
| `cli/CliOptions.cpp` | CLI 解析实现。处理 `--transport`、`--host`、`--port`、`--sdp`、`--decoder`、`--renderer` 及 UDP 相关参数 |

## 4. 核心类 / 函数说明

### main() 函数

作用：
- 程序唯一入口
- 串起整个客户端管线
- 管理三个线程的生命周期

关键流程：
- `ParseClientOption()`：解析命令行
- `StreamClient::Connect()`：建立网络连接
- `VideoDecoder::Initialize()`：初始化解码器
- `VideoRenderer::Initialize()`：初始化渲染器
- 启动 `receive_thread`：持续调用 `client.ReceiveFrame()`
- 启动 `decode_thread`：持续调用 `decoder.Decode()`
- 主线程循环：`renderer.PollKey()` → `renderer.Render()` → 记录延迟

### PipelineDecodedFrame 结构体

作用：
- 管线内部传递的带时间戳的解码帧
- 封装了 `DecodedFrame`、`FrameDiagnosticMetadata` 和各阶段时间戳
- 用于计算 `receive_to_decode`、`decode_time`、`decode_to_render`、`receive_to_render` 等延迟

### ParseClientOption()

作用：
- 解析客户端命令行参数
- 填充 `ClientConfig`、`DecodeBackend`、`RenderBackend` 等配置
- 支持共享参数（`ParseSharedStreamOption`）和客户端特有参数

### ParseSharedStreamOption()

作用：
- 解析网络和 UDP 相关的共享参数
- 被 `ParseClientOption` 和 `ParseUdpBenchmarkOption` 共用

## 5. 数据流说明

输入：
- 命令行参数（`argc/argv`）

处理：
- CLI 解析 → `ClientConfig`
- SDP 加载（可选）→ 覆盖 host/port/payload_type/clock_rate
- 初始化三大模块
- 启动管线

输出：
- 接收线程：`ReceivedFrame` → `received_frames` 队列
- 解码线程：`PipelineDecodedFrame` → `decoded_frames` 队列
- 渲染线程：OpenGL 窗口画面 + HUD 叠加
- 退出时：延迟统计摘要输出到日志

## 6. 与其他模块的关系

```
main.cpp
  ├── 调用 app/cli/ 解析参数
  ├── 调用 modules/network/StreamClient 建立连接、接收帧
  ├── 调用 modules/decoding/VideoDecoder 解码 H.264
  ├── 调用 modules/rendering/VideoRenderer 渲染画面
  ├── 使用 common/concurrency/BoundedQueue 传递帧
  ├── 使用 common/metrics/LatencyStats 统计延迟
  ├── 使用 common/log/Logger 输出日志
  ├── 使用 common/net/SdpSessionDescription 解析 SDP
  └── 使用 common/net/H264AnnexB 检测 IDR 帧
```

## 7. 线程模型 / 队列模型

本模块管理三个线程：

| 线程 | 职责 | 队列 |
|---|---|---|
| `receive_thread` | 调用 `StreamClient::ReceiveFrame()` 持续接收帧 | 写入 `received_frames`（容量默认 8） |
| `decode_thread` | 从 `received_frames` 取帧，调用 `VideoDecoder::Decode()` | 写入 `decoded_frames`（容量默认 3） |
| 主线程 | 渲染循环，从 `decoded_frames` 取最新帧渲染 | 读取 `decoded_frames` |

队列策略：
- `BoundedQueue::PushOrDropOldest()`：队列满时丢弃最旧帧，避免阻塞生产者
- 解码线程有额外的背压控制：当 `decoded_frames` 队列满且当前帧不是 IDR 时，直接跳过

停止机制：
- `std::atomic_bool stop_requested`：信号处理函数设置
- `BoundedQueue::Close()`：关闭队列，使阻塞的 `WaitPop()` 返回 false
- 接收失败时自动重连（最多 10 次，间隔 1 秒）

线程安全：
- `pipeline_state_mutex`：保护 `pipeline_error_message` 和 `latest_udp_stats`
- `send_mutex`（在 StreamClient 内部）：保护 socket 发送

## 8. 配置参数

通过命令行传入的主要参数：

| 参数 | 默认值 | 说明 |
|---|---|---|
| `--transport` | `tcp` | 传输协议：tcp/udp/rtp |
| `--host` | `127.0.0.1` | 服务端地址（RTP 模式下为本地 bind 地址） |
| `--port` | `9999` | 端口 |
| `--sdp` | 空 | SDP 文件路径，加载后自动切到 rtp 模式 |
| `--decoder` | `auto` | 解码后端：auto/software |
| `--renderer` | `auto` | 渲染后端：auto/opengl |
| `--vsync` | `off` | 是否开启垂直同步 |
| `--receive-queue` | `8` | 接收队列容量 |
| `--decode-queue` | `3` | 解码队列容量 |
| `--metadata` | `on` | 是否期望发送端延迟元数据 |

## 9. 调试建议

- **启动失败**：检查日志中 `failed to connect`、`failed to initialize decoder`、`failed to initialize renderer` 对应的错误信息
- **收不到帧**：关注 `receive failed` 和 `reconnecting...` 日志
- **解码失败**：关注 `non-existing PPS` 警告，通常是中途加入缺少关键帧
- **延迟统计**：每 120 帧自动输出一次延迟摘要，或退出时输出最终摘要
- **适合打断点的位置**：
  - `main.cpp:199`：`client.Connect()` 之后，确认连接成功
  - `main.cpp:274`：`client.ReceiveFrame()` 返回后，确认收到帧
  - `main.cpp:325`：`decoder.Decode()` 返回后，确认解码成功
  - `main.cpp:417`：`renderer.Render()` 返回后，确认渲染成功

## 10. 后续扩展方向

- 增加配置文件支持（当前仅命令行）
- 增加音频管线
- 增加更多渲染后端（Vulkan、Metal）
- 增加硬件解码后端（VAAPI、NVDEC）
- 增加管线状态机（暂停、快进、seek）
- 增加录制功能
