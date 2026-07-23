# common/log

## 1. 模块定位

日志系统。提供线程安全的日志输出，同时写入 stdout/stderr 和日志文件。

## 2. 核心职责

- 提供 Debug/Info/Warn/Error 四个日志级别
- 日志同时输出到控制台和文件
- 自动创建 `logs/` 目录和带时间戳的日志文件
- 线程安全，支持多线程并发写入

## 3. 主要文件说明

| 文件 | 作用 |
|---|---|
| Logger.h | 定义 `Logger` 类和 `LogLevel` 枚举 |
| Logger.cpp | 实现日志格式化、文件管理、线程安全输出 |

## 4. 核心类 / 函数说明

### Logger

作用：
- 全静态类，所有方法通过 `Logger::Info(...)` 等静态调用
- 内部维护一个全局 `std::mutex` 保护所有输出操作

关键函数：
- `SetVerbose(bool)`：控制 Debug 级别是否输出（默认 true）
- `CurrentLogFilePath()`：返回当前日志文件路径
- `Debug()` / `Info()` / `Warn()` / `Error()`：输出对应级别的日志

日志格式：
```
2024-01-15 10:30:45 [INFO] application started
```

日志文件命名：
```
logs/<executable_name>_<YYYYMMDD_HHMMSS>_<pid>.log
```

日志目录解析逻辑：
- 如果可执行文件位于 `build/` 目录下，则日志写入项目根目录的 `logs/`
- 否则写入当前工作目录的 `logs/`

## 5. 数据流说明

输入：
- 日志消息字符串
- 日志级别

处理：
- 格式化时间戳和级别前缀
- 按行分割多行消息
- 同时输出到 stdout/stderr 和日志文件

输出：
- 控制台输出
- 日志文件

## 6. 与其他模块的关系

- 被所有模块使用
- 不依赖任何业务模块

## 7. 线程模型

所有 `Log()` 调用在内部通过 `std::mutex` 串行化。多线程并发调用是安全的，但会互相阻塞。

## 8. 调试建议

- 如果日志文件未创建，检查 `logs/` 目录的写入权限
- 如果 Debug 日志不输出，检查 `SetVerbose(true)` 是否被调用
- 日志文件路径在程序启动时会输出一行 "log file: ..." 日志
