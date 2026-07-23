# src/common/log

## 1. 模块定位

日志模块。提供统一的日志输出能力，同时输出到控制台和文件，被整个项目的所有模块使用。

## 2. 核心职责

- 提供 Debug/Info/Wind/Error 四级日志
- 同时输出到控制台（stdout/stderr）和 `logs/` 目录下的日志文件
- 线程安全（内部加锁）
- 日志文件自动按日期和 PID 命名
- Error 输出到 stderr，其他输出到 stdout
- Debug 级别受 verbose 开关控制

## 3. 主要文件说明

| 文件 | 作用 |
|---|---|
| `Logger.h` | 日志接口声明：`SetVerbose()`、`Debug()`/`Info()`/`Warn()`/`Error()`、`CurrentLogFilePath()` |
| `Logger.cpp` | 日志实现：格式化前缀、文件初始化、双路输出 |

## 4. 核心类 / 函数说明

### Logger

作用：
- 全静态类，所有方法为 static
- 被项目中所有模块调用

关键函数：
- `SetVerbose(bool)`：控制 Debug 级别是否输出
- `Debug/Info/Wind/Error(message)`：输出对应级别的日志
- `CurrentLogFilePath()`：获取当前日志文件路径
- `Log(level, message)`：内部实现，格式化前缀 + 双路输出

日志格式：
```
2024-01-15 10:30:45 [INFO] message content
```

## 5. 数据流说明

输入：
- 各模块调用 `Logger::Info()` 等传入字符串消息

输出：
- 控制台（stdout 或 stderr）
- 文件：`logs/{程序名}_{YYYYMMDD_HHMMSS}_{PID}.log`

## 6. 与其他模块的关系

- 被所有模块使用，是项目中最基础的依赖
- `main.cpp` 中用于输出延迟统计摘要
- `StreamClient` 中用于输出连接状态和错误
- `AdaptiveJitterBuffer` 中用于输出 jitter buffer 模式切换

## 7. 线程模型 / 队列模型

- 使用 `std::mutex` 保护日志输出的原子性
- 使用 `std::atomic<bool>` 控制 verbose 开关
- 日志文件初始化使用 `static bool` 标志，首次调用时自动创建

## 8. 配置参数

- `Logger::SetVerbose(true/false)`：控制 Debug 级别输出（默认 true）
- 日志目录：固定为当前工作目录下的 `logs/`

## 9. 调试建议

- 检查日志文件：`logs/` 目录下会自动创建，文件名含日期和 PID
- 检查控制台输出：Error 级别走 stderr，其他走 stdout
- 如果日志文件创建失败，会在 stderr 输出一条警告

## 10. 后续扩展方向

- 增加日志级别运行时动态调整
- 增加结构化日志（JSON 格式）
- 增加日志轮转（按大小或日期）
- 增加异步日志（减少锁竞争）
