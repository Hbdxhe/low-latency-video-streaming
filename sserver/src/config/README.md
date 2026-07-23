# config

## 1. 模块定位

配置模型与加载校验层。定义所有可配置参数的数据结构，提供从 `.conf` 文件加载配置的能力，以及配置合法性校验。

## 2. 核心职责

- 定义 `AppConfig` 及其子结构体（`RuntimeConfig`、`CaptureConfig`、`TransportConfig`、`CodecConfig`）
- 提供 `ConfigLoader::LoadFromFile()` 从 key=value 格式的 `.conf` 文件加载配置
- 提供 `AppConfig::Validate()` 校验配置合法性
- 提供 `AppConfig::CreateDefault()` 生成默认配置

## 3. 主要文件说明

| 文件 | 作用 |
|---|---|
| AppConfig.h | 定义 `RuntimeConfig`、`CaptureConfig`、`TransportConfig`、`CodecConfig`、`AppConfig` 结构体和 `ConfigLoader` 类 |
| AppConfig.cpp | 实现配置文件解析（key=value 格式）、配置校验逻辑 |

## 4. 核心类 / 函数说明

### AppConfig

作用：
- 聚合所有配置子结构体，作为全局配置的单一入口
- 每个子结构体都有默认值，`CreateDefault()` 返回一个全默认的配置实例

关键结构体：

#### RuntimeConfig
- `shutdown_grace_period_ms`：关闭优雅期（默认 500ms）
- `latency_log_interval_frames`：每隔多少帧输出一次延迟统计（默认 120）

#### CaptureConfig
- `enabled`：是否启用采集（默认 true）
- `source`：采集源类型，`"v4l2"` 或 `"null"`（默认 `"v4l2"`）
- `device`：V4L2 设备路径（默认 `"/dev/video0"`）
- `width` / `height` / `fps`：采集分辨率和帧率（默认 640x360@30fps）
- `frame_interval_ms`：Null 设备的帧间隔（默认 0）
- `device_buffer_count`：V4L2 内核缓冲区数量（默认 2）
- `null_payload_bytes`：Null 设备填充 payload 大小
- `null_payload_mode`：Null 设备模式，`"text"` 或 `"h264_test_pattern"`

#### TransportConfig
- `enabled`：是否启用传输（默认 true）
- `backend`：传输后端，`"tcp"` / `"udp"` / `"rtp"`（默认 `"tcp"`）
- `bind_address` / `listen_port`：监听地址和端口（默认 `0.0.0.0:9999`）
- `max_pending_frames`：TCP 发送队列最大帧数（默认 3）
- `max_queue_wait_ms`：TCP 队列最大等待时间，超过则丢弃非关键帧（默认 50ms）
- `queue_drop_policy`：队列丢弃策略，`"drop_oldest"` 或 `"drop_oldest_non_key"`（默认后者）
- `enable_nodelay`：TCP 是否启用 `TCP_NODELAY`（默认 true）
- `embed_frame_metadata`：是否在协议头中嵌入帧诊断元数据（默认 false）
- `udp_*`：UDP 相关参数（缓冲区大小、分片大小、NACK、FEC 等）
- `rtp_*`：RTP 相关参数（远程地址、payload type、clock rate、SSRC、max payload size 等）

#### CodecConfig
- `backend`：编码后端，当前仅支持 `"x264"`
- `x264_preset` / `x264_tune` / `x264_profile`：x264 编码预设
- `x264_keyint_max` / `x264_keyint_min`：关键帧间隔
- `x264_bframes`：B 帧数量（默认 0，低延迟场景）
- `x264_threads` / `x264_sliced_threads`：线程配置
- 其他 x264 细粒度参数

### ConfigLoader

作用：
- 从 `.conf` 文件加载配置，支持 `#` 注释和 `key = value` 格式
- 遇到未知 key 直接报错（严格模式）
- 加载完成后自动调用 `Validate()` 校验

关键函数：
- `LoadFromFile(file_path, config, error_message)`：从文件加载配置，返回是否成功

## 5. 数据流说明

输入：
- `.conf` 配置文件路径（通过命令行参数 `--config` 指定）

处理：
- 逐行解析 `key = value` 格式
- 类型转换（bool、int、size_t、string）
- 调用 `Validate()` 校验参数合法性

输出：
- `AppConfig` 实例，传递给 `ApplicationContext`，再传递给各模块

## 6. 与其他模块的关系

- 被 `app/main.cpp` 调用加载配置
- `AppConfig` 被 `core/ApplicationContext` 持有
- `AppConfig` 被所有业务模块读取
- `AppConfig.cpp` 中调用 `CaptureBackendSelection`、`TransportBackendSelection`、`VideoEncoderBackendSelection` 的解析函数进行校验，形成对 modules 层的弱依赖

## 7. 线程模型 / 队列模型

本模块不涉及线程。配置加载和校验都是同步操作，在程序启动时一次性完成。

## 8. 配置参数

本模块本身就是配置参数的定义者。所有配置参数详见 `AppConfig.h` 中的结构体定义和 `config/default.conf` 中的示例。

配置文件格式：
```
# 注释行
key = value
```

支持的值类型：
- 布尔：`true`/`false`/`1`/`0`/`yes`/`no`/`on`/`off`
- 整数：十进制整数
- 大小：无符号整数
- 字符串：直接写值，不需要引号

## 9. 调试建议

- 配置加载失败时，`error_message` 会包含具体行号和原因
- 常见错误：未知 key、类型不匹配、值超出合法范围
- 可以使用 `AppConfig::CreateDefault()` 生成默认配置进行对比
- 校验逻辑在 `AppConfig::Validate()` 中，可以打断点观察校验过程
- 配置文件中 `transport.backend` 的合法值为 `tcp`/`udp`/`rtp`，`capture.source` 的合法值为 `v4l2`/`null`

## 10. 后续扩展方向

- 支持 JSON / YAML 格式配置
- 支持环境变量覆盖配置项
- 支持配置热重载
- 支持配置模板和继承
- 增加更多传输后端的配置参数（如 SRT、WebRTC）
