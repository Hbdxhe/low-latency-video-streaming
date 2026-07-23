#ifndef SSERVER_COMMON_LOG_LOGGER_H
#define SSERVER_COMMON_LOG_LOGGER_H

#include <string>

namespace sserver {
namespace common {
namespace log {

// 日志级别。
enum class LogLevel {
    kDebug,
    kInfo,
    kWarn,
    kError,
};

// 线程安全的进程级日志工具。
//
// 日志同时写终端和文件，并自动添加时间与级别前缀。
class Logger {
public:
    // 控制 Debug 日志是否输出。
    static void SetVerbose(bool verbose);
    // 返回当前日志文件路径，并触发日志文件懒初始化。
    static std::string CurrentLogFilePath();

    // 各级别便捷接口。
    static void Debug(const std::string &message);
    static void Info(const std::string &message);
    static void Warn(const std::string &message);
    static void Error(const std::string &message);

private:
    // 所有级别最终进入统一实现。
    static void Log(LogLevel level, const std::string &message);
};

}  // namespace log
}  // namespace common
}  // namespace sserver

#endif  // SSERVER_COMMON_LOG_LOGGER_H
