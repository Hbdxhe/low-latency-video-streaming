#include "common/log/Logger.h"

#include <atomic>
#include <cerrno>
#include <chrono>
#include <ctime>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <limits.h>
#include <mutex>
#include <sstream>
#include <string>
#include <sys/stat.h>
#include <unistd.h>

namespace sclient {
namespace common {
namespace log {

namespace {

// 全局状态，使用函数内静态变量确保线程安全的初始化

std::mutex &LogMutex() {
    static std::mutex mutex;
    return mutex;
}

std::atomic<bool> &VerboseFlag() {
    static std::atomic<bool> verbose{true};
    return verbose;
}

std::ofstream &LogFileStream() {
    static std::ofstream stream;
    return stream;
}

std::string &LogFilePath() {
    static std::string path;
    return path;
}

bool &LogFileInitialized() {
    static bool initialized = false;
    return initialized;
}

bool &LogFileWarningPrinted() {
    static bool printed = false;
    return printed;
}

/** 将日志级别转换为字符串标识 */
const char *ToString(LogLevel level) {
    switch (level) {
        case LogLevel::kDebug:
            return "DEBUG";
        case LogLevel::kInfo:
            return "INFO";
        case LogLevel::kWarn:
            return "WARN";
        case LogLevel::kError:
            return "ERROR";
        default:
            return "UNKNOWN";
    }
}

/**
 * 格式化日志前缀
 * 格式：YYYY-MM-DD HH:MM:SS [LEVEL]
 */
std::string FormatPrefix(LogLevel level) {
    const auto now = std::chrono::system_clock::now();
    const std::time_t now_time = std::chrono::system_clock::to_time_t(now);

    std::tm time_info{};
    localtime_r(&now_time, &time_info);

    std::ostringstream stream;
    stream << std::put_time(&time_info, "%F %T");
    stream << " [" << ToString(level) << "] ";
    return stream.str();
}

/** 根据日志级别选择输出流：错误级别用stderr，其他用stdout */
std::ostream &OutputStreamForLevel(LogLevel level) {
    return level == LogLevel::kError ? std::cerr : std::cout;
}

/** 获取可执行文件的基本名称（不含路径） */
std::string ExecutableBaseName() {
    char buffer[PATH_MAX] = {0};
    const ssize_t size = readlink("/proc/self/exe", buffer, sizeof(buffer) - 1);
    if (size <= 0) {
        return "sclient";
    }

    buffer[size] = '\0';
    const std::string full_path(buffer);
    const std::size_t separator = full_path.find_last_of('/');
    if (separator == std::string::npos || separator + 1 >= full_path.size()) {
        return full_path;
    }
    return full_path.substr(separator + 1);
}

/** 确保日志目录存在，不存在则创建 */
bool EnsureLogDirectory() {
    char cwd[PATH_MAX] = {0};
    if (getcwd(cwd, sizeof(cwd)) == nullptr) {
        if (mkdir("logs", 0755) == 0) {
            return true;
        }
        return errno == EEXIST;
    }
    const std::string logs_path = std::string(cwd) + "/logs";
    if (mkdir(logs_path.c_str(), 0755) == 0) {
        return true;
    }
    return errno == EEXIST;
}

/** 获取日志目录的绝对路径 */
std::string LogsDirectoryPath() {
    char cwd[PATH_MAX] = {0};
    if (getcwd(cwd, sizeof(cwd)) != nullptr) {
        return std::string(cwd) + "/logs";
    }
    return "logs";
}

/**
 * 初始化日志文件
 *
 * 首次调用时创建日志文件，后续调用直接返回。
 * 文件名格式：可执行文件名_YYYYMMDD_HHMMSS_PID.log
 */
void EnsureLogFileInitialized() {
    if (LogFileInitialized()) {
        return;
    }
    LogFileInitialized() = true;

    if (!EnsureLogDirectory()) {
        if (!LogFileWarningPrinted()) {
            std::cerr << "failed to create logs directory; file logging disabled" << std::endl;
            LogFileWarningPrinted() = true;
        }
        return;
    }

    const auto now = std::chrono::system_clock::now();
    const std::time_t now_time = std::chrono::system_clock::to_time_t(now);
    std::tm time_info{};
    localtime_r(&now_time, &time_info);

    std::ostringstream filename;
    filename << LogsDirectoryPath()
             << "/"
             << ExecutableBaseName()
             << "_"
             << std::put_time(&time_info, "%Y%m%d_%H%M%S")
             << "_"
             << static_cast<long>(getpid())
             << ".log";

    LogFilePath() = filename.str();
    LogFileStream().open(LogFilePath().c_str(), std::ios::out | std::ios::app);
    if (!LogFileStream().is_open()) {
        if (!LogFileWarningPrinted()) {
            std::cerr << "failed to open log file '" << LogFilePath() << "'; file logging disabled" << std::endl;
            LogFileWarningPrinted() = true;
        }
        LogFilePath().clear();
    }
}

}  // namespace

void Logger::SetVerbose(bool verbose) {
    VerboseFlag() = verbose;
}

std::string Logger::CurrentLogFilePath() {
    std::lock_guard<std::mutex> lock(LogMutex());
    EnsureLogFileInitialized();
    return LogFilePath();
}

void Logger::Debug(const std::string &message) {
    Log(LogLevel::kDebug, message);
}

void Logger::Info(const std::string &message) {
    Log(LogLevel::kInfo, message);
}

void Logger::Warn(const std::string &message) {
    Log(LogLevel::kWarn, message);
}

void Logger::Error(const std::string &message) {
    Log(LogLevel::kError, message);
}

/**
 * 核心日志输出函数
 *
 * 处理多行消息，每行都添加时间戳和级别前缀。
 * 同时输出到控制台和日志文件（如果文件已打开）。
 *
 * 注意：调试级别日志在非详细模式下会被静默丢弃
 */
void Logger::Log(LogLevel level, const std::string &message) {
    // 调试级别日志在非详细模式下不输出
    if (level == LogLevel::kDebug && !VerboseFlag()) {
        return;
    }

    const std::string prefix = FormatPrefix(level);
    std::lock_guard<std::mutex> lock(LogMutex());
    std::ostream &output = OutputStreamForLevel(level);
    EnsureLogFileInitialized();
    std::ofstream &file_output = LogFileStream();

    // 按行分割消息，每行独立添加前缀
    std::size_t start = 0;
    while (start <= message.size()) {
        const std::size_t end = message.find('\n', start);
        const std::string line = end == std::string::npos
                ? message.substr(start)
                : message.substr(start, end - start);
        output << prefix << line << std::endl;
        if (file_output.is_open()) {
            file_output << prefix << line << std::endl;
        }

        if (end == std::string::npos) {
            break;
        }
        start = end + 1;
    }
}

}  // namespace log
}  // namespace common
}  // namespace sclient
