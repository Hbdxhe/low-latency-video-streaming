# tests/smoke

## 1. 模块定位

烟雾测试。验证应用配置加载、模块初始化、启动和停止的完整生命周期不崩溃。

## 2. 核心职责

- 加载配置文件
- 初始化并启动 `AppBootstrap`
- 等待 120ms
- 停止应用
- 验证整个过程无崩溃、无异常退出

## 3. 主要文件说明

| 文件 | 作用 |
|---|---|
| SmokeTest.cpp | 烟雾测试主程序。加载配置 → 初始化 → 启动 → sleep 120ms → 停止，返回 0 表示成功 |

## 4. 测试逻辑

1. 解析 `--config` 参数（默认 `config/smoke.conf`）
2. `ConfigLoader::LoadFromFile()` 加载配置
3. `AppBootstrap::Initialize()` 初始化所有模块
4. `AppBootstrap::Start()` 启动所有模块
5. `sleep_for(120ms)` 等待模块运行
6. `AppBootstrap::Stop()` 停止所有模块
7. 返回 0

## 5. 返回值

| 返回值 | 含义 |
|---|---|
| 0 | 成功 |
| 1 | 配置加载失败 |
| 2 | 初始化失败 |
| 3 | 启动失败 |

## 6. CTest 配置

- `smoke_test`：使用 `config/smoke.conf`（TCP 后端，Null 设备），超时 10s
- `smoke_udp_test`：使用 `config/smoke_udp.conf`（UDP 后端，Null 设备），超时 10s
