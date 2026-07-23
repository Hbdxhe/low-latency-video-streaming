# tests

## 1. 模块定位

测试与基准测试目录。包含烟雾测试、集成测试、单元测试、传输基准测试和端到端延迟基准测试，覆盖配置加载、模块生命周期、协议解析、传输链路和端到端延迟等维度。

## 2. 测试分层

| 层级 | 子目录 | 说明 | 硬件依赖 | 超时 |
|---|---|---|---|---|
| smoke | smoke/ | 验证配置加载、应用初始化、模块生命周期 | 无 | 10s |
| integration | integration/ | 验证 null capture → transport → client 端到端链路 | 无 | 15-25s |
| unit | unit/ | 验证协议解析、配置校验、模块回滚、延迟统计等独立逻辑 | 无 | 10s |
| benchmark | benchmark/ | 基于 null capture 的轻量传输基准，输出 capture_to_send 和 capture_to_receive 统计 | 无 | 20-30s |
| latency | latency/ | 真实链路延迟基准，补充 decode/present_ready 指标 | V4L2 可选 | 25-30s |

## 3. 子目录说明

| 子目录 | 作用 |
|---|---|
| smoke/ | 烟雾测试：启动应用、等待 120ms、停止应用，验证模块生命周期不崩溃 |
| integration/ | 集成测试：启动完整链路，连接客户端，验证帧接收、协议头校验、时间戳一致性、NACK/FEC 恢复 |
| benchmark/ | 传输基准测试：接收 120 帧，分阶段统计 capture_to_encode_start、encode_time、encode_to_send、capture_to_send、network_to_receive、capture_to_receive |
| latency/ | 端到端延迟基准测试：在 benchmark 基础上增加 FFmpeg 解码和可选 OpenCV 渲染，统计 capture_to_decode 和 capture_to_present_ready |
| unit/ | 单元测试：RTP 协议解析、UDP/RTP 配置校验、配置加载负测试、Application 生命周期回滚、LatencyRecorder 统计准确性、UDP FEC 恢复 |
| support/ | 测试辅助工具：`TransportTestClient.h` 提供 TCP/UDP 帧接收、KeepAlive 发送、NACK 发送、UDP 分片重组、FEC 恢复等公共函数 |

## 4. 测试配置文件

所有测试通过 `--config` 参数指定配置文件，配置文件位于 `config/` 目录：

| 配置文件 | 用途 |
|---|---|
| smoke.conf | 烟雾测试（TCP） |
| smoke_udp.conf | 烟雾测试（UDP） |
| integration_tcp.conf | TCP 集成测试 |
| integration_udp.conf | UDP 集成测试 |
| integration_udp_fec.conf | UDP FEC 集成测试 |
| integration_udp_fec_nack.conf | UDP FEC+NACK 集成测试 |
| integration_rtp_null.conf | RTP 集成测试（Null 设备） |
| integration_rtp_v4l2.conf | RTP 集成测试（V4L2 设备） |
| benchmark_null_tcp.conf | TCP 传输基准（Null 设备） |
| benchmark_null_udp.conf | UDP 传输基准（Null 设备） |
| benchmark_null_rtp.conf | RTP 传输基准（Null 设备） |
| benchmark.conf | 延迟基准（V4L2 + TCP） |
| benchmark_udp.conf | 延迟基准（V4L2 + UDP） |
| benchmark_rtp_null.conf | 延迟基准（Null + RTP） |
| benchmark_rtp_v4l2.conf | 延迟基准（V4L2 + RTP） |

## 5. 运行测试

```bash
# 构建
cd build && cmake .. && make

# 运行所有测试
ctest

# 运行特定标签的测试
ctest -L smoke
ctest -L unit
ctest -L integration
ctest -L benchmark
ctest -L latency

# 运行单个测试
ctest -R smoke_test
ctest -R rtp_protocol_test
```

## 6. CTest 测试注册

| 测试名 | 可执行文件 | 配置文件 | 标签 |
|---|---|---|---|
| smoke_test | smoke_test | smoke.conf | smoke |
| smoke_udp_test | smoke_test | smoke_udp.conf | smoke;udp |
| integration_transport_tcp | transport_integration_test | integration_tcp.conf | integration |
| integration_transport_udp | transport_integration_test | integration_udp.conf | integration |
| integration_transport_udp_fec | transport_integration_test | integration_udp_fec.conf | integration |
| integration_transport_udp_fec_nack | transport_integration_test | integration_udp_fec_nack.conf | integration;udp |
| integration_rtp_null | rtp_integration_test | integration_rtp_null.conf | integration;rtp |
| integration_rtp_v4l2 | rtp_integration_test | integration_rtp_v4l2.conf | integration;rtp;hardware |
| rtp_protocol_test | rtp_protocol_test | — | unit;rtp |
| rtp_config_test | rtp_config_test | — | unit;rtp |
| udp_config_test | udp_config_test | — | unit;udp |
| udp_profile_config_test | udp_profile_config_test | — | unit;udp |
| config_loader_negative_test | config_loader_negative_test | — | unit;config |
| udp_fec_recovery_test | udp_fec_recovery_test | — | unit;udp;fec |
| application_lifecycle_test | application_lifecycle_test | — | unit;core |
| latency_recorder_test | latency_recorder_test | — | unit;metrics |
| benchmark_transport_tcp | transport_benchmark | benchmark_null_tcp.conf | benchmark |
| benchmark_transport_udp | transport_benchmark | benchmark_null_udp.conf | benchmark |
| benchmark_transport_rtp | transport_benchmark | benchmark_null_rtp.conf | benchmark;rtp |
| latency_benchmark_rtp_null | latency_benchmark | benchmark_rtp_null.conf | latency;rtp |
| latency_benchmark_tcp | latency_benchmark | benchmark.conf | latency;tcp;hardware |
| latency_benchmark_udp | latency_benchmark | benchmark_udp.conf | latency;udp;hardware |
| latency_benchmark_rtp_v4l2 | latency_benchmark | benchmark_rtp_v4l2.conf | latency;rtp;hardware |

## 7. 与 src/ 模块的关系

- smoke/integration/benchmark/latency 测试通过 `AppBootstrap` 启动完整应用链路
- unit 测试直接测试 `common/`、`config/`、`core/` 的独立组件
- `support/TransportTestClient.h` 复用了 `common/net/StreamProtocol.h` 的协议结构
