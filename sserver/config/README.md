# config

## 1. 目录定位

配置文件目录。存放所有 `.conf` 格式的运行配置文件，覆盖默认运行、测试、基准测试和客户端预设等场景。

## 2. 配置文件格式

所有配置文件使用 `key = value` 格式，支持 `#` 注释行。

```
# 注释
app.name = sserver
capture.source = v4l2
transport.backend = tcp
```

配置加载逻辑见 `src/config/AppConfig.cpp` 中的 `ConfigLoader::LoadFromFile()`。

## 3. 配置文件分类

### 默认运行配置

| 文件 | 说明 |
|---|---|
| default.conf | 默认配置。V4L2 采集 640x480@30fps，TCP 传输端口 9999，x264 ultrafast/zerolatency/baseline |

### 客户端预设配置（sclient_*）

用于服务端与客户端（sclient）联调时的预设配置，固定端口，V4L2 采集。

| 文件 | 端口 | 后端 | NACK | FEC | target_payload | 说明 |
|---|---|---|---|---|---|---|
| sclient_tcp.conf | 19099 | TCP | — | — | 1200 | TCP 联调 |
| sclient_udp.conf | 19100 | UDP | 关 | 关 | 65000 | UDP balanced 预设，裸发最低延迟 |
| sclient_udp_adaptive.conf | 19101 | UDP | 开 | 开 | 1000 | UDP adaptive 预设，NACK+FEC，小分片 |
| sclient_udp_resilient.conf | 19102 | UDP | 开 | 开 | 256 | UDP resilient 预设，NACK+FEC+重传缓存，极小分片 |

### 烟雾测试配置（smoke_*）

用于 `ctest -L smoke`，Null 设备，端口 0（系统自动分配），最短生命周期。

| 文件 | 后端 | 说明 |
|---|---|---|
| smoke.conf | TCP | TCP 烟雾测试 |
| smoke_udp.conf | UDP | UDP 烟雾测试 |

### 集成测试配置（integration_*）

用于 `ctest -L integration`，Null 设备（除 rtp_v4l2），端口 0，`embed_frame_metadata=true`。

| 文件 | 后端 | 设备 | NACK | FEC | target_payload | 说明 |
|---|---|---|---|---|---|---|
| integration_tcp.conf | TCP | null | — | — | 1200 | TCP 集成测试 |
| integration_udp.conf | UDP | null | 关 | 关 | 65000 | UDP 集成测试 |
| integration_udp_fec.conf | UDP | null | 关 | 开 | 256 | UDP FEC 集成测试 |
| integration_udp_fec_nack.conf | UDP | null | 开 | 开 | 256 | UDP FEC+NACK 集成测试 |
| integration_rtp_null.conf | RTP | null(h264) | — | — | 320 | RTP 集成测试，端口 19504 |
| integration_rtp_v4l2.conf | RTP | V4L2 | — | — | 1200 | RTP 集成测试，端口 19514，需要硬件 |

### 传输基准测试配置（benchmark_null_*）

用于 `ctest -L benchmark`，Null 设备，端口 0，`frame_interval_ms=1`（最快速率），`embed_frame_metadata=true`。

| 文件 | 后端 | queue_drop_policy | NACK | FEC | 说明 |
|---|---|---|---|---|---|
| benchmark_null_tcp.conf | TCP | drop_oldest_non_key | — | — | TCP 基准 |
| benchmark_null_tcp_drop_oldest.conf | TCP | drop_oldest | — | — | TCP 基准（丢弃最旧策略） |
| benchmark_null_tcp_drop_oldest_non_key.conf | TCP | drop_oldest_non_key | — | — | TCP 基准（丢弃最旧非关键帧策略） |
| benchmark_null_udp.conf | UDP | — | 关 | 关 | UDP 基准 |
| benchmark_null_udp_fast.conf | UDP | — | 关 | 关 | UDP 快速基准 |
| benchmark_null_rtp.conf | RTP(h264) | — | — | — | RTP 基准，端口 19604 |

### 延迟基准测试配置（benchmark_*, benchmark_rtp_*）

用于 `ctest -L latency`，V4L2 或 Null 设备，端口 0。

| 文件 | 后端 | 设备 | 端口 | 说明 |
|---|---|---|---|---|
| benchmark.conf | TCP | V4L2 | 0 | 延迟基准（TCP + V4L2），需要硬件 |
| benchmark_udp.conf | UDP | V4L2 | 0 | 延迟基准（UDP + V4L2），需要硬件 |
| benchmark_rtp_null.conf | RTP | null(h264) | 19604 | 延迟基准（RTP + Null） |
| benchmark_rtp_v4l2.conf | RTP | V4L2 | 19614 | 延迟基准（RTP + V4L2），需要硬件 |

### 负测试配置（benchmark_invalid_*）

用于 `ctest` 的 `WILL_FAIL` 测试，验证延迟基准在缺少必要配置时正确失败。

| 文件 | 后端 | 故意缺少的配置 | 说明 |
|---|---|---|---|
| benchmark_invalid_udp_no_metadata.conf | UDP | `embed_frame_metadata=false` | UDP 延迟基准缺少帧元数据，应失败 |
| benchmark_invalid_rtp_no_extension.conf | RTP | `rtp_enable_latency_extension=false` | RTP 延迟基准缺少延迟扩展头，应失败 |

## 4. 关键配置项差异对比

### 采集源差异

| 场景 | capture.source | capture.null_payload_mode | 说明 |
|---|---|---|---|
| 真实摄像头 | v4l2 | — | 需要 /dev/videoX 设备 |
| 测试模式（纯文本） | null | text | 生成 "null-frame-N" 文本 payload |
| 测试模式（H.264） | null | h264_test_pattern | 合成 YUYV 帧 → x264 编码 |

### 传输后端差异

| 场景 | transport.backend | embed_frame_metadata | rtp_enable_latency_extension | 说明 |
|---|---|---|---|---|
| TCP | tcp | true | — | 帧元数据嵌入 MessageHeader 之后 |
| UDP | udp | true | — | 帧元数据嵌入 UdpFrameFragmentHeader |
| RTP | rtp | false | true | 时间戳通过 RTP 延迟扩展头携带 |

### UDP 丢包恢复差异

| 场景 | udp_enable_nack | udp_enable_fec | udp_target_payload_size | 说明 |
|---|---|---|---|---|
| 裸发 | false | false | 65000 | 最低延迟，不做任何恢复 |
| FEC | false | true | 256 | 前向纠错，小分片增加 FEC 覆盖率 |
| NACK | true | false | 65000 | 重传，依赖客户端反馈 |
| FEC+NACK | true | true | 256 | 全开，最高容错 |

## 5. 与代码的关系

- 所有配置文件通过 `--config` 参数传递给 `stream_server` 或测试可执行文件
- 配置加载逻辑在 `src/config/AppConfig.cpp` 的 `ConfigLoader::LoadFromFile()`
- 配置校验逻辑在 `AppConfig::Validate()`
- 各模块从 `ApplicationContext::config` 中读取所需配置

## 6. 如何添加新配置

1. 在 `src/config/AppConfig.h` 中添加新的配置字段和默认值
2. 在 `src/config/AppConfig.cpp` 的 `LoadFromFile()` 中添加 key 解析分支
3. 在 `AppConfig::Validate()` 中添加校验逻辑
4. 在 `config/default.conf` 中添加配置项和注释
5. 根据需要更新测试配置文件
