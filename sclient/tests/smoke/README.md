# tests/smoke

## 1. 模块定位

冒烟测试目录。验证最小 RTP 接收 + 解码链路是否能跑通，是 CI 中最核心的端到端验证。

## 2. 核心职责

- 使用进程内自带的 RTP sender 发送 H.264 SPS/PPS/IDR 样本
- 自动选择本地空闲 UDP 端口
- 验证 `StreamClient + VideoDecoder` 能完成最小解码链路
- 支持切换到外部 RTP 流联调模式

## 3. 主要文件说明

| 文件 | 作用 |
|---|---|
| `RtpDecodeSmoke.cpp` | RTP 解码冒烟测试：进程内 sender + 接收 + 解码验证 |

## 4. 核心流程

1. 解析命令行参数
2. 如果 `--self-test on`（默认）：
   - 自动选择本地空闲 UDP 端口
   - 启动 sender 线程，发送内嵌的 H.264 SPS/PPS/IDR 帧
3. `StreamClient::Connect()` 连接到本地端口
4. `VideoDecoder::Initialize()` 初始化解码器
5. 循环：`ReceiveFrame()` → `Decode()` → 统计解码帧数
6. 验证解码帧数达到目标（默认 3 帧）
7. 输出结果

## 5. 内嵌 H.264 样本

使用 `tests/support/H264Samples.h` 中的内嵌数据：
- 由 `ffmpeg` 生成的 64x48 红色帧
- 包含 SPS（type=7）、PPS（type=8）、IDR（type=5）
- 使用 `EmbeddedH264IdrFrameNalus()` 获取分离的 NALU 列表

## 6. 命令行参数

| 参数 | 默认值 | 说明 |
|---|---|---|
| `--host` | `127.0.0.1` | 本地 bind 地址 |
| `--port` | 自动选择 | 本地 bind 端口 |
| `--frames` | `3` | 目标解码帧数 |
| `--max-receive` | `12` | 最大接收帧数 |
| `--decoder` | `auto` | 解码后端 |
| `--self-test` | `on` | 是否使用进程内 sender |
| `--help` | | 显示帮助 |

## 7. 与其他模块的关系

- 调用 `modules/network/StreamClient` 接收 RTP
- 调用 `modules/decoding/VideoDecoder` 解码 H.264
- 使用 `tests/support/H264Samples.h` 的内嵌样本
- 使用 `common/net/RtpProtocol.h` 构造 RTP 包
- 使用 `app/cli/CliOptions.h` 解析参数

## 8. 运行方式

```bash
# 自测模式（默认）
ctest --test-dir build --output-on-failure -L smoke
./build/rtp_decode_smoke

# 外部 RTP 流模式
./build/rtp_decode_smoke --self-test off --host 127.0.0.1 --port 19514 --frames 30
```

超时限制：10 秒。
