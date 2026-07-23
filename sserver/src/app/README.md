# app

## 1. 模块定位

程序入口与启动编排层。负责解析命令行参数、加载配置、创建并启动 Application 实例、注册模块、绑定跨模块数据流、处理信号退出。

## 2. 核心职责

- 解析 `--config` 命令行参数，确定配置文件路径
- 加载并校验配置文件（通过 `ConfigLoader::LoadFromFile`）
- 注册 SIGINT/SIGTERM 信号处理，实现优雅退出
- 创建 `AppBootstrap`，负责模块注册、初始化、启动和停止
- 绑定 capture → transport 之间的数据流（`FrameHandler` 回调）
- 输出启动/关闭摘要日志

## 3. 主要文件说明

| 文件 | 作用 |
|---|---|
| main.cpp | 程序入口。解析命令行参数、加载配置、注册信号处理、启动 `AppBootstrap`、主循环等待退出信号 |
| AppBootstrap.h | 定义 `AppBootstrap` 类，负责模块注册、数据流绑定、启动/停止编排 |
| AppBootstrap.cpp | 实现模块创建（`CaptureModule`、`TransportModule`）、`BindStreamingPipeline` 绑定回调、启动/停止/关闭流程 |

## 4. 核心类 / 函数说明

### AppBootstrap

作用：
- 持有 `core::Application` 实例，负责模块的注册和生命周期管理
- 持有 `LatencyRecorder`，用于记录从采集到发送的端到端延迟
- 通过 `BindStreamingPipeline()` 将 `CaptureModule` 的帧输出回调绑定到 `TransportModule::Broadcast()`

关键函数：
- `Initialize()`：创建 `TransportModule` 和 `CaptureModule`，注册到 `Application`，调用 `Application::Initialize()`
- `Start()`：调用 `Initialize()` → `BindStreamingPipeline()` → `Application::Start()`，打印启动摘要
- `Stop()`：`UnbindStreamingPipeline()` → `Application::Stop()` → `Application::Shutdown()`，打印关闭摘要
- `BindStreamingPipeline()`：设置 `capture_module_->SetFrameHandler(lambda)`，lambda 内调用 `transport_module->Broadcast(frame)`
- `bound_port()`：返回传输模块实际监听的端口号

### main() 中的信号处理

- 注册 `SIGINT`、`SIGTERM` 处理函数，设置 `g_exit_requested` 原子标志
- 主循环每 200ms 检查一次退出标志
- 忽略 `SIGPIPE`，避免 TCP 写入 broken pipe 时崩溃

## 5. 数据流说明

输入：
- 命令行参数 `--config <path>`
- 配置文件（默认 `config/default.conf`）
- SIGINT / SIGTERM 信号

处理：
- 配置加载 → 模块创建 → 模块注册 → 数据流绑定 → 启动 → 等待退出信号

输出：
- 启动摘要日志（app_name、capture_source、transport_backend、listen_port）
- 关闭摘要日志

## 6. 与其他模块的关系

- 创建并持有 `CaptureModule`（`src/modules/capture/`）
- 创建并持有 `TransportModule`（`src/modules/transport/`）
- 通过 `core::Application`（`src/core/`）管理模块生命周期
- 使用 `AppConfig`（`src/config/`）获取配置
- 使用 `LatencyRecorder`（`src/common/metrics/`）记录延迟
- 使用 `Logger`（`src/common/log/`）输出日志

## 7. 线程模型 / 队列模型

`AppBootstrap` 本身不直接管理线程。它负责绑定 `CaptureModule` 和 `TransportModule` 之间的回调关系。

主循环在 `main()` 中以 200ms 间隔轮询退出标志。`AppBootstrap::Start()` 是同步调用，模块内部线程由各模块自行管理。

## 8. 配置参数

`AppBootstrap` 不直接读取配置参数，而是将 `AppConfig` 传递给 `Application`，由各模块自行从 `ApplicationContext` 中获取所需配置。

启动时使用的配置文件路径通过 `--config` 命令行参数指定，默认为 `config/default.conf`。

## 9. 调试建议

- 启动时观察日志输出的 "application started" 行，确认 app_name、capture_source、transport_backend、listen_port 是否正确
- 如果启动失败，检查 "failed to initialize module" 或 "failed to start module" 日志，定位是哪个模块初始化/启动失败
- 退出时观察 "shutdown requested by signal" 和 "application stopped" 日志确认优雅退出
- 如果程序启动后立即退出，检查配置文件路径是否正确、配置校验是否通过

## 10. 后续扩展方向

- 支持更多命令行参数（如覆盖配置项、设置日志级别）
- 支持热重载配置
- 支持更多模块的动态注册（如音频采集、录制、网关等）
- 将 `BindStreamingPipeline()` 从同步回调升级为异步事件总线
