#include "common/log/Logger.h"

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

namespace sserver {
namespace common {
namespace log {

namespace {

// 函数内静态对象保证全进程只使用同一把日志锁。
std::mutex &LogMutex() {
    static std::mutex mutex;
    return mutex;
}

bool &VerboseFlag() {
    // 默认输出 Debug；SetVerbose(false) 可关闭。
    static bool verbose = true;
    return verbose;
}

std::ofstream &LogFileStream() {
    // 日志文件流按需初始化，全进程共享。
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

const char *ToString(LogLevel level) {
    // 将枚举转换成日志前缀中的可读文本。
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

std::string FormatPrefix(LogLevel level) {
    // 使用系统墙上时钟生成便于人阅读的日期时间。
    // 延迟测量不能使用这个时钟，而应使用 MonotonicNowNs。
    const auto now = std::chrono::system_clock::now();

    // 转成传统 C 时间
    const std::time_t now_time = std::chrono::system_clock::to_time_t(now);

    // 转成本地年月日时分秒
    std::tm time_info{};
    localtime_r(&now_time, &time_info);

    std::ostringstream stream;
    stream << std::put_time(&time_info, "%F %T");
    stream << " [" << ToString(level) << "] ";
    return stream.str();
}

// 选择终端输出流
std::ostream &OutputStreamForLevel(LogLevel level) {
    // Error 写 stderr，其余级别写 stdout。
    return level == LogLevel::kError ? std::cerr : std::cout;
}

std::string ExecutableBaseName() {
    // /proc/self/exe 是 Linux 当前进程可执行文件的符号链接。
    // 文件名用于生成 stream_server_时间_进程号.log。
    char buffer[PATH_MAX] = {0};
    const ssize_t size = readlink("/proc/self/exe", buffer, sizeof(buffer) - 1);
    if (size <= 0) {
        return "stream_server";
    }

    buffer[size] = '\0';
    const std::string full_path(buffer);
    const std::size_t separator = full_path.find_last_of('/');
    if (separator == std::string::npos || separator + 1 >= full_path.size()) {
        return full_path;
    }
    return full_path.substr(separator + 1);
}

std::string ExecutablePath() {
    // 获取当前可执行文件绝对路径。
    char buffer[PATH_MAX] = {0};
    const ssize_t size = readlink("/proc/self/exe", buffer, sizeof(buffer) - 1);
    if (size <= 0) {
        return std::string();
    }

    buffer[size] = '\0';
    return std::string(buffer);
}

std::string ParentDirectory(const std::string &path) {
    // 简单获取父目录，不依赖 filesystem，以保持 C++14 兼容。
    const std::size_t separator = path.find_last_of('/');
    if (separator == std::string::npos) {
        return std::string();
    }
    if (separator == 0) {
        return "/";
    }
    return path.substr(0, separator);
}

std::string BaseName(const std::string &path) {
    // 简单获取路径最后一段。
    const std::size_t separator = path.find_last_of('/');
    if (separator == std::string::npos) {
        return path;
    }
    if (separator + 1 >= path.size()) {
        return std::string();
    }
    return path.substr(separator + 1);
}

// 确定日志目录
std::string ResolveLogDirectory() {
    // 如果程序位于 sserver/build/，则日志写到 sserver/logs/。
    // 其他运行布局退化为当前工作目录下的 logs/。
    const std::string executable_path = ExecutablePath();
    if (!executable_path.empty()) {
        const std::string executable_dir = ParentDirectory(executable_path);
        if (!executable_dir.empty()) {
            const std::string maybe_build_dir = BaseName(executable_dir);
            if (maybe_build_dir == "build") {
                const std::string project_root_dir = ParentDirectory(executable_dir);
                if (!project_root_dir.empty()) {
                    return project_root_dir + "/logs";
                }
            }
        }
    }
    return "logs";
}

bool EnsureLogDirectory(const std::string &log_directory) {
    // mkdir 成功或目录已经存在都视为可用。
    if (log_directory.empty()) {
        return false;
    }

    if (mkdir(log_directory.c_str(), 0755) == 0) {
        return true;
    }
    return errno == EEXIST;
}

// 创建日志文件
void EnsureLogFileInitialized() {
    // 懒初始化：首次查询路径或首次写日志时才创建日志文件。
    if (LogFileInitialized()) {
        return;
    }
    // 即使创建失败也不反复尝试，避免每条日志重复输出错误。
    LogFileInitialized() = true;

    const std::string log_directory = ResolveLogDirectory();
    if (!EnsureLogDirectory(log_directory)) {
        if (!LogFileWarningPrinted()) {
            std::cerr << "failed to create logs directory '" << log_directory
                      << "'; file logging disabled" << std::endl;
            LogFileWarningPrinted() = true;
        }
        return;
    }

    const auto now = std::chrono::system_clock::now();
    const std::time_t now_time = std::chrono::system_clock::to_time_t(now);
    std::tm time_info{};
    localtime_r(&now_time, &time_info);

    // 文件名包含可执行文件名、时间和 PID，避免并行进程互相覆盖。
    std::ostringstream filename;
    filename << log_directory
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
    // 该项目通常在启动阶段设置此标志；运行中并发修改时没有额外同步。
    VerboseFlag() = verbose;
}

std::string Logger::CurrentLogFilePath() {
    // 初始化与路径读取都放在日志锁内。
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

void Logger::Log(LogLevel level, const std::string &message) {
    // verbose 关闭时跳过 Debug。
    if (level == LogLevel::kDebug && !VerboseFlag()) {
        return;
    }

    const std::string prefix = FormatPrefix(level);
    // 保证不同线程的日志行不会在终端和文件中交叉。
    std::lock_guard<std::mutex> lock(LogMutex());
    std::ostream &output = OutputStreamForLevel(level);
    EnsureLogFileInitialized();
    std::ofstream &file_output = LogFileStream();

    // 多行 message 拆成多条日志，每一行都带完整前缀。
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
}  // namespace sserver
