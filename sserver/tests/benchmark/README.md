# tests/benchmark

## 1. 模块定位

传输基准测试。基于 null capture 的轻量基准，接收 120 帧后输出分阶段延迟统计。

## 2. 主要文件说明

| 文件 | 作用 |
|---|---|
| TransportBenchmark.cpp | 传输基准测试主程序。支持 TCP/UDP/RTP 三种后端，支持 `--receive-delay-ms` 模拟客户端延迟 |

## 3. 测试逻辑

1. 解析参数：`--config`（配置文件）、`--receive-delay-ms`（接收延迟）
2. 加载配置，校验 `embed_frame_metadata=true`（TCP/UDP）或 `rtp_enable_latency_extension=true`（RTP）
3. 启动 `AppBootstrap`
4. 连接客户端（TCP/UDP）或打开 RTP 接收 socket
5. 循环接收 120 帧，每帧计算：
   - `capture_to_encode_start`：采集到编码开始
   - `encode_time`：编码耗时
   - `encode_to_send`：编码完成到发送
   - `send_to_receive`：发送到接收（网络延迟）
   - `capture_to_receive`：端到端延迟
6. 关闭连接，停止应用
7. 输出各阶段的 count/min/avg/p50/p95/p99/max 统计

## 4. 命令行参数

| 参数 | 说明 | 默认值 |
|---|---|---|
| --config | 配置文件路径 | config/benchmark_null_tcp.conf |
| --receive-delay-ms | 每帧接收后的延迟（毫秒），用于模拟慢客户端 | 0 |

## 5. 输出指标

- `capture_to_encode_start`：采集到编码开始的延迟
- `encode_time`：编码耗时
- `encode_to_send`：编码完成到传输层发送的延迟
- `capture_to_send`：采集到发送的端到端延迟（服务端侧）
- `network_to_receive`：发送到接收的网络延迟
- `capture_to_receive`：采集到接收的端到端延迟

## 6. CTest 配置

| 测试名 | 配置文件 | 标签 | 超时 |
|---|---|---|---|
| benchmark_transport_tcp | benchmark_null_tcp.conf | benchmark | 20s |
| benchmark_transport_tcp_drop_oldest | benchmark_null_tcp_drop_oldest.conf | benchmark | 30s |
| benchmark_transport_tcp_drop_oldest_non_key | benchmark_null_tcp_drop_oldest_non_key.conf | benchmark | 30s |
| benchmark_transport_udp | benchmark_null_udp.conf | benchmark | 20s |
| benchmark_transport_rtp | benchmark_null_rtp.conf | benchmark;rtp | 20s |
