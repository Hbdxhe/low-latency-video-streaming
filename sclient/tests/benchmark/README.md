# tests/benchmark

## 1. 模块定位

基准测试目录。提供 UDP 接收、抖动缓冲和恢复统计的观测工具，用于性能分析和回归检测。

## 2. 核心职责

- 接收指定数量的 UDP 帧
- 统计各阶段延迟（network→receive、receive→release、capture→release、decode time）
- 统计 UDP 恢复指标（完成率、超时帧、跳过帧、FEC 恢复、NACK 请求）
- 输出格式化的统计报告

## 3. 主要文件说明

| 文件 | 作用 |
|---|---|
| `UdpJitterBenchmark.cpp` | UDP 抖动基准测试工具 |

## 4. 核心流程

1. 解析命令行参数（使用 `ParseUdpBenchmarkOption`）
2. `StreamClient::Connect()` 连接 UDP 服务端
3. 如果 `--decode on`：初始化 `VideoDecoder`
4. 循环接收指定帧数：`ReceiveFrame()` → 可选 `Decode()` → 记录延迟
5. 输出统计报告：
   - benchmark 配置摘要
   - recovery 统计（完成率、超时、跳过、FEC、NACK）
   - 各阶段延迟统计（min/avg/p50/p95/p99/max）
   - UDP 接收详细统计

## 5. 命令行参数

| 参数 | 默认值 | 说明 |
|---|---|---|
| `--host` | `127.0.0.1` | 服务端地址 |
| `--port` | `9999` | 端口 |
| `--transport` | `udp` | 传输协议 |
| `--frames` | `240` | 测量帧数 |
| `--decode` | `off` | 是否解码 |
| `--decoder` | `auto` | 解码后端 |
| `--udp-jitter-buffer` | `on` | jitter buffer |
| `--udp-jitter-buffer-strategy` | `auto` | 策略 |
| `--udp-nack` | `on` | NACK |
| `--udp-fec` | `on` | FEC |
| `--inject-loss-pattern` | `none` | 丢包注入 |
| `--inject-jitter-pattern` | `none` | 抖动注入 |
| `--help` | | 显示帮助 |

## 6. 输出示例

```
udp_jitter_benchmark host=127.0.0.1 port=19100 frames=240 released=240 decoded=0 ...
udp_recovery completion_rate=99.58% completed_frames=239 timed_out_frames=1 ...
network_to_receive count=240 min=0.12ms avg=0.45ms p50=0.38ms p95=0.89ms p99=1.23ms max=2.10ms last=0.40ms
receive_to_release count=240 min=0.01ms avg=0.02ms p50=0.02ms p95=0.03ms p99=0.05ms max=0.10ms last=0.02ms
...
udp_receive datagrams=480 fragments=480 completed_frames=239 ...
```

## 7. 与其他模块的关系

- 调用 `modules/network/StreamClient` 接收 UDP
- 调用 `modules/decoding/VideoDecoder`（可选）
- 使用 `common/metrics/LatencyStats` 统计延迟
- 使用 `app/cli/CliOptions.h` 解析参数

## 8. 运行方式

```bash
# 只看帮助
./build/udp_jitter_benchmark --help

# 基本用法（需要配合 sserver 的 UDP 流）
./build/udp_jitter_benchmark --frames 120

# 带解码
./build/udp_jitter_benchmark --frames 120 --decode on

# 带丢包注入
./build/udp_jitter_benchmark --frames 120 --inject-loss-pattern single --inject-loss-period 30
```

ctest 中注册的是 `--help` 检查，真正的 benchmark 按需手动运行。
