# src/app/cli

## 1. 模块定位

命令行参数解析子模块。负责将 `argc/argv` 解析为结构化的配置对象，供 `main.cpp` 使用。

## 2. 核心职责

- 解析客户端命令行参数（`ParseClientOption`）
- 解析 UDP 基准测试参数（`ParseUdpBenchmarkOption`）
- 解析共享的网络/UDP 参数（`ParseSharedStreamOption`）
- 输出使用帮助（`PrintClientUsage`、`PrintUdpBenchmarkUsage`）
- 类型转换：布尔值解析、解码后端解析、渲染后端解析

## 3. 主要文件说明

| 文件 | 作用 |
|---|---|
| `CliOptions.h` | CLI 解析接口声明 |
| `CliOptions.cpp` | CLI 解析实现 |

## 4. 核心类 / 函数说明

### CliParseResult

作用：
- 解析结果结构体
- 包含 `handled`（是否处理了参数）、`success`（是否成功）、`show_help`（是否显示帮助）、`error_message`

### ParseClientOption()

作用：
- 解析客户端特有的参数：`--transport`、`--renderer`、`--vsync`、`--window-title`、`--receive-queue`、`--decode-queue`
- 内部调用 `ParseSharedStreamOption()` 处理共享参数

### ParseSharedStreamOption()

作用：
- 解析网络和 UDP 相关的共享参数
- 被 `ParseClientOption` 和 `ParseUdpBenchmarkOption` 共用
- 处理：`--host`、`--port`、`--sdp`、`--metadata`、`--udp-jitter-buffer-*`、`--udp-nack-*`、`--udp-fec-*`、`--inject-loss-*`、`--decoder`、`--help`

### ParseBoolFlag()

作用：
- 解析布尔值字符串：`on/true/1` → true，`off/false/0` → false

### ParseDecodeBackend() / ParseRenderBackend()

作用：
- 解析后端选择字符串为枚举值

## 5. 数据流说明

输入：
- `argc/argv`：命令行参数

输出：
- `ClientConfig`：填充网络和 UDP 配置
- `DecodeBackend`：解码后端选择
- `RenderBackend`：渲染后端选择
- `renderer_vsync_enabled`：是否开启 vsync
- `window_title`：窗口标题
- `receive_queue_capacity` / `decode_queue_capacity`：队列容量

## 6. 与其他模块的关系

- 被 `app/main.cpp` 调用
- 使用 `modules/network/types/ClientConfig.h` 的配置结构体
- 使用 `modules/decoding/VideoDecoder.h` 的 `DecodeBackend` 枚举
- 使用 `modules/rendering/VideoRenderer.h` 的 `RenderBackend` 枚举
- 使用 `common/log/Logger` 输出帮助信息和错误

## 7. 线程模型 / 队列模型

本模块当前不直接管理线程。所有解析函数在主线程中调用，无状态。

## 8. 配置参数

本模块本身就是配置参数的解析器。所有支持的参数见 `ClientConfig` 和 `PrintClientUsage()` 的输出。

## 9. 调试建议

- 参数解析失败：查看返回的 `CliParseResult.error_message`
- 未知参数：返回 `handled=false`，`main.cpp` 会打印使用帮助
- 适合打断点：`ParseClientOption()` 入口处

## 10. 后续扩展方向

- 增加配置文件支持（TOML/YAML/JSON）
- 增加环境变量支持
- 增加参数校验（值范围检查）
- 增加参数自动补全
