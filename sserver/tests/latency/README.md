# tests/latency

## 1. 模块定位

端到端延迟基准测试。在传输基准基础上增加 FFmpeg H.264 解码和可选 OpenCV 渲染，统计从采集到解码和呈现的完整延迟链路。

## 2. 主要文件说明

| 文件 | 作用 |
|---|---|
| LatencyBenchmark.cpp | 延迟基准测试主程序。接收 120 帧，每帧送入 FFmpeg 解码器，统计分阶段延迟 |
| DecodeDisplayProbe.h | `DecodeDisplayProbe` 类定义，封装 FFmpeg 解码和 OpenCV 渲染 |
| DecodeDisplayProbe.cpp | 实现 FFmpeg H.264 解码（`avcodec_send_packet` / `avcodec_receive_frame`），可选 OpenCV 窗口显示 |

## 3. DecodeDisplayProbe 类

作用：
- 封装 FFmpeg H.264 软件解码器
- 可选 OpenCV BGR 转换和窗口显示
- 记录解码延迟和呈现延迟

关键函数：
- `Initialize(show_window, error_message)`：初始化 FFmpeg 解码器（`avcodec_find_decoder(AV_CODEC_ID_H264)`）
- `DecodeAndPresent(data, size, capture_timestamp_ns, error_message)`：解码一帧 H.264 数据，记录 decode_latency 和 present_latency
- `Shutdown()`：释放 FFmpeg 和 OpenCV 资源
- `decode_latency_recorder()`：返回解码延迟统计
- `present_latency_recorder()`：返回呈现延迟统计

## 4. 测试逻辑

1. 解析参数：`--config`、`--show-window`
2. 加载配置，校验延迟扩展头或帧元数据启用
3. V4L2 设备不可用时返回 77（跳过）
4. 启动 `AppBootstrap`
5. 连接客户端或打开 RTP 接收 socket
6. 初始化 `DecodeDisplayProbe`
7. 循环接收并解码 120 帧：
   - 接收帧（TCP/UDP/RTP）
   - 计算各阶段延迟
   - `DecodeAndPresent()` 解码并可选显示
   - TCP/UDP 模式下每 15 帧发送 KeepAlive
8. 输出统计：
   - `capture_to_encode_start`
   - `encode_time`
   - `encode_to_send`
   - `capture_to_send`
   - `network_to_receive`
   - `capture_to_receive`
   - `capture_to_decode`
   - `capture_to_present_ready`

## 5. 命令行参数

| 参数 | 说明 | 默认值 |
|---|---|---|
| --config | 配置文件路径 | config/benchmark.conf |
| --show-window | 启用 OpenCV 窗口显示 | 不启用 |

## 6. 依赖

- FFmpeg（libavcodec、libavutil、libswscale）：必需
- OpenCV（opencv4）：可选，用于窗口显示。编译时通过 `SSERVER_HAS_OPENCV` 宏控制

## 7. CTest 配置

| 测试名 | 配置文件 | 标签 | 超时 | 备注 |
|---|---|---|---|---|
| latency_benchmark_rtp_null | benchmark_rtp_null.conf | latency;rtp | 25s | 无需硬件 |
| latency_benchmark_tcp | benchmark.conf | latency;tcp;hardware | 30s | 需要 V4L2 |
| latency_benchmark_udp | benchmark_udp.conf | latency;udp;hardware | 30s | 需要 V4L2 |
| latency_benchmark_rtp_v4l2 | benchmark_rtp_v4l2.conf | latency;rtp;hardware | 30s | 需要 V4L2 |
| latency_benchmark_invalid_udp_no_metadata | benchmark_invalid_udp_no_metadata.conf | latency;negative;udp | 10s | 预期失败 |
| latency_benchmark_invalid_rtp_no_extension | benchmark_invalid_rtp_no_extension.conf | latency;negative;rtp | 10s | 预期失败 |
