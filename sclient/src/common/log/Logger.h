#ifndef SCLIENT_COMMON_LOG_LOGGER_H
#define SCLIENT_COMMON_LOG_LOGGER_H

#include <string>

namespace sclient {
namespace common {
namespace log {

/** 日志级别 */
enum class LogLevel {
    kDebug,  /**< 调试信息，默认不输出到控制台 */
    kInfo,   /**< 普通信息 */
    kWarn,   /**< 警告信息 */
    kError,  /**< 错误信息，输出到stderr */
};

/**
 * 线程安全的日志工具类
 *
 * 支持同时输出到控制台和日志文件。
 * 日志文件位于 ./logs/ 目录下，文件名格式为：可执行文件名_时间_PID.log
 */
class Logger {
public:
    /** 设置是否输出调试级别日志 */
    static void SetVerbose(bool verbose);
    /** 获取当前日志文件路径 */
    static std::string CurrentLogFilePath();

    static void Debug(const std::string &message);
    static void Info(const std::string &message);
    static void Warn(const std::string &message);
    static void Error(const std::string &message);

private:
    static void Log(LogLevel level, const std::string &message);
};

}  // namespace log
}  // namespace common
}  // namespace sclient

#endif  // SCLIENT_COMMON_LOG_LOGGER_H
