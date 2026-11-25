#include "app/cli/CliOptions.h"

#include <algorithm>
#include <cerrno>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <sstream>
#include <string>

#include "common/log/Logger.h"

namespace sclient {

namespace {

using common::log::Logger;

/** 安全解析浮点数，失败返回 false */
bool ParseDouble(const char *value, double *result) {
    // value 为空、result 为空、字符串为空，都不能解析
    if (value == nullptr || result == nullptr || value[0] == '\0') {
        return false;
    }
    char *end = nullptr;

    // 清空 errno，方便后面判断 strtod 是否出错
    errno = 0;

    // 把字符串转换成 double
    const double parsed = std::strtod(value, &end);

    // 判断失败条件：
    // errno != 0：转换过程出错，比如溢出。
    // end == value：一个字符都没解析出来。
    // *end != '\0'：后面还有无法解析的垃圾字符，比如 "1.2abc"。
    // strtod also accepts nan/inf; configuration values must be finite.
    if (errno != 0 || end == value || *end != '\0' || !std::isfinite(parsed)) {
        return false;
    }
    *result = parsed;
    return true;
}

/** 安全解析整数，失败返回 false */
bool ParseInt(const char *value, int *result) {
    if (value == nullptr || result == nullptr || value[0] == '\0') {
        return false;
    }
    char *end = nullptr;
    errno = 0;

    // 按 10 进制解析 long
    const long parsed = std::strtol(value, &end, 10);

    if (errno != 0 || end == value || *end != '\0') {
        return false;
    }

    *result = static_cast<int>(parsed);
    return true;
}

/** 构建参数值无效的解析结果 */
// 例如：--port abc
CliParseResult BuildInvalidValueResult(const std::string &option_name) {
    CliParseResult result;

    // handled=true 表示这个参数已经被识别了
    result.handled = true;

    // success=false 表示虽然识别了参数，但解析失败
    result.success = false;
    result.error_message = "invalid value for " + option_name;
    return result;
}

/** 构建缺少参数值的解析结果 */
// 例如：用户只写 --host，没有跟 IP
CliParseResult BuildMissingValueResult(const std::string &option_name) {
    CliParseResult result;

    // 参数已被识别
    result.handled = true;

    // 解析失败
    result.success = false;
    result.error_message = "missing value for " + option_name;
    return result;
}

/** 构建解析成功的结果 */
CliParseResult BuildHandledSuccess() {
    CliParseResult result;
    result.handled = true;
    return result;
}

/** 构建显示帮助的结果 */
CliParseResult BuildHelpResult() {
    CliParseResult result;
    result.handled = true;
    result.show_help = true;
    return result;
}

/** 追加流媒体通用选项的使用说明 */
void AppendSharedStreamUsage(std::ostringstream *stream) {
    if (stream == nullptr) {
        return;
    }

    // 往 usage 文本里追加通用参数说明
    (*stream)
            // 服务端 IP，默认本机。
            << "  --host <ip>                 default: 127.0.0.1\n"
            // 服务端端口，默认 9999。
            << "  --port <port>               default: 9999\n"
            // RTP 模式可以从 SDP 文件里加载地址、端口、payload type、clock rate。
            << "  --sdp <path>                load RTP bind address/port from SDP\n"
            // 是否期望服务端发送诊断元数据，比如采集时间、发送时间。
            << "  --metadata <on|off>         default: on\n"
            // UDP 是否启用 jitter buffer，用来缓解网络抖动。
            << "  --udp-jitter-buffer <on|off> default: on\n"
            // UDP jitter buffer 策略。
            << "  --udp-jitter-buffer-strategy <auto|off|low|smooth|fixed|adaptive> default: auto\n"
            // jitter buffer 目标延迟，单位 ms。
            << "  --udp-jitter-buffer-target-ms <ms> default: 8\n"
            // jitter buffer 最小延迟，单位 ms。
            << "  --udp-jitter-buffer-min-ms <ms>   default: 2\n"
            // jitter buffer 安全系数，例如根据抖动估计乘以 1.5。
            << "  --udp-jitter-buffer-safety <factor> default: 1.5\n"
            // 自适应 jitter buffer 最大延迟。
            << "  --udp-jitter-buffer-adaptive-max-ms <ms> default: 120\n"
            // 单帧最大等待时间。
            << "  --udp-jitter-buffer-max-wait-ms <ms> default: 80\n"
            // jitter buffer 最多缓存帧数。
            << "  --udp-jitter-buffer-max-frames <n> default: 8\n"
            // UDP 是否开启 NACK 重传。
            << "  --udp-nack <on|off>         default: on\n"
            // UDP 是否开启 FEC 前向纠错。
            << "  --udp-fec <on|off>          default: on\n"
            // 等待多久发现缺包后发送 NACK。
            << "  --udp-nack-delay-ms <ms>    default: 12\n"
            // NACK 重试间隔。
            << "  --udp-nack-retry-ms <ms>    default: 20\n"
            // NACK 最大重试次数。
            << "  --udp-nack-max-retries <n>  default: 3\n"
            // 测试用：模拟丢包模式。
            << "  --inject-loss-pattern <none|single|burst|alternate> default: none\n"
            // 测试用：每隔多少个包/分片触发一次模拟丢包。
            << "  --inject-loss-period <n>    default: 30\n"
            // 测试用：每次模拟丢多少个包/分片。
            << "  --inject-loss-count <n>     default: 1\n"
            // 解码后端选择。
            << "  --decoder <auto|off>        default: auto\n";
}

/** 解析 on/off 开关选项 */
CliParseResult ParseOnOffOption(
        int argc,
        char **argv,
        int *index,
        const char *option_name,
        bool *target) {

    // 防御性检查
    if (index == nullptr || option_name == nullptr) {
        CliParseResult result;
        result.handled = true;
        result.success = false;
        result.error_message = "internal cli parser error";
        return result;
    }

    // 如果当前参数后面没有值，说明缺少 on/off
    if (*index + 1 >= argc) {
        return BuildMissingValueResult(option_name);
    }

    // 先把 index 移动到参数值位置，再解析 bool。
    // 例如 argv[index] 是 "--udp-nack"，argv[index+1] 是 "on"。
    if (!ParseBoolFlag(argv[++(*index)], target)) {
        return BuildInvalidValueResult(option_name);
    }
    return BuildHandledSuccess();
}

}  // namespace

// 解析布尔字符串
bool ParseBoolFlag(const char *value, bool *result) {
    if (value == nullptr || result == nullptr) {
        return false;
    }
    const std::string normalized(value);
    if (normalized == "on" || normalized == "true" || normalized == "1") {
        *result = true;
        return true;
    }
    if (normalized == "off" || normalized == "false" || normalized == "0") {
        *result = false;
        return true;
    }
    return false;
}

// 解析解码后端参数
bool ParseDecodeBackend(const char *value, DecodeBackend *result) {
    if (value == nullptr || result == nullptr) {
        return false;
    }

    // 转成 string 比较
    const std::string normalized(value);

    // auto 表示自动选择解码后端
    if (normalized == "auto") {
        *result = DecodeBackend::kAuto;
        return true;
    }

    // software 表示软件解码，通常底层是 FFmpeg
    if (normalized == "software") {
        *result = DecodeBackend::kSoftware;
        return true;
    }
    return false;
}

// 解析渲染后端参数
bool ParseRenderBackend(const char *value, RenderBackend *result) {
    if (value == nullptr || result == nullptr) {
        return false;
    }

    // 转成 string 比较
    const std::string normalized(value);

    // auto 表示自动选择渲染后端
    if (normalized == "auto") {
        *result = RenderBackend::kAuto;
        return true;
    }

    // opengl 表示使用 OpenGL 渲染
    if (normalized == "opengl") {
        *result = RenderBackend::kOpenGl;
        return true;
    }
    return false;
}

// 打印普通客户端使用说明
void PrintClientUsage(const char *program_name) {
    // 用 ostringstream 拼接多行 usage 文本
    std::ostringstream stream;
    stream << "Usage: " << program_name << " [options]\n"
           << "  --transport <tcp|udp|rtp>   default: tcp\n";

    // 追加 host/port/udp/decoder 等通用参数
    AppendSharedStreamUsage(&stream);

    // 追加普通客户端专属参数
    stream << "  --renderer <auto|opengl>    default: auto\n"
           << "  --vsync <on|off>            default: off\n"
           << "  --window-title <title>      default: sclient\n"
           << "  --receive-queue <n>         default: 8\n"
           << "  --decode-queue <n>          default: 3\n"
           << "  --help";
    Logger::Info(stream.str());
}

// 打印 UDP benchmark 工具使用说明
void PrintUdpBenchmarkUsage(const char *program_name) {
    std::ostringstream stream;

    // benchmark 专属参数：测多少帧，是否解码
    stream << "Usage: " << program_name << " [options]\n"
           << "  --frames <n>                default: 240\n"
           << "  --decode <on|off>           default: off\n";

    // benchmark 也复用流媒体通用参数
    AppendSharedStreamUsage(&stream);

    // benchmark 额外支持模拟 jitter
    stream << "  --inject-jitter-pattern <none|saw|burst|alternate> default: none\n"
           << "  --inject-jitter-amplitude-ms <ms> default: 0\n"
           << "  --inject-jitter-period <n>  default: 6\n"
           << "  --help";
    Logger::Info(stream.str());
}

// 解析客户端和 benchmark 都会用到的通用流媒体参数
CliParseResult ParseSharedStreamOption(
        int argc,
        char **argv,
        int *index,
        ClientConfig *config,
        DecodeBackend *decode_backend) {
    if (index == nullptr || config == nullptr || decode_backend == nullptr) {
        CliParseResult result;
        result.handled = true;
        result.success = false;
        result.error_message = "internal cli parser error";
        return result;
    }

    // 解析 --host <ip>
    if (std::strcmp(argv[*index], "--host") == 0) {
        // 如果 --host 后面没有 IP，返回缺值错误
        if (*index + 1 >= argc) {
            return BuildMissingValueResult("--host");
        }
        config->host = argv[++(*index)];
        return BuildHandledSuccess();
    }

    // 解析 --port <port>
    if (std::strcmp(argv[*index], "--port") == 0) {
        if (*index + 1 >= argc) {
            return BuildMissingValueResult("--port");
        }
        if (!ParseInt(argv[++(*index)], &config->port)) {
            return BuildInvalidValueResult("--port");
        }
        return BuildHandledSuccess();
    }

    // 解析 --sdp <path>
    if (std::strcmp(argv[*index], "--sdp") == 0) {
        if (*index + 1 >= argc) {
            return BuildMissingValueResult("--sdp");
        }
        config->sdp_path = argv[++(*index)];
        return BuildHandledSuccess();
    }

    // 解析 --metadata <on|off>
    if (std::strcmp(argv[*index], "--metadata") == 0) {
        // 写入 config->expect_metadata。
        return ParseOnOffOption(argc, argv, index, "--metadata", &config->expect_metadata);
    }

    // 解析 --udp-jitter-buffer <on|off>
    if (std::strcmp(argv[*index], "--udp-jitter-buffer") == 0) {
        // 控制 UDP 是否启用抖动缓冲
        return ParseOnOffOption(argc, argv, index, "--udp-jitter-buffer", &config->udp_jitter_buffer_enabled);
    }

    // 解析 --udp-jitter-buffer-strategy <...>
    if (std::strcmp(argv[*index], "--udp-jitter-buffer-strategy") == 0) {
        if (*index + 1 >= argc) {
            return BuildMissingValueResult("--udp-jitter-buffer-strategy");
        }

        // 保存策略字符串
        config->udp_jitter_buffer_strategy = argv[++(*index)];

        // 校验策略是否在允许范围内
        if (config->udp_jitter_buffer_strategy != "fixed" &&
            config->udp_jitter_buffer_strategy != "adaptive" &&
            config->udp_jitter_buffer_strategy != "auto" &&
            config->udp_jitter_buffer_strategy != "off" &&
            config->udp_jitter_buffer_strategy != "low" &&
            config->udp_jitter_buffer_strategy != "smooth") {
            return BuildInvalidValueResult("--udp-jitter-buffer-strategy");
        }
        return BuildHandledSuccess();
    }

    // 解析 --udp-jitter-buffer-target-ms <ms>
    if (std::strcmp(argv[*index], "--udp-jitter-buffer-target-ms") == 0) {
        // 检查是否缺值
        if (*index + 1 >= argc) {
            return BuildMissingValueResult("--udp-jitter-buffer-target-ms");
        }

        // 解析目标缓冲延迟
        if (!ParseInt(argv[++(*index)], &config->udp_jitter_buffer_target_delay_ms)) {
            return BuildInvalidValueResult("--udp-jitter-buffer-target-ms");
        }
        return BuildHandledSuccess();
    }

    // 解析 --udp-jitter-buffer-adaptive-max-ms <ms>
    if (std::strcmp(argv[*index], "--udp-jitter-buffer-adaptive-max-ms") == 0) {
        if (*index + 1 >= argc) {
            return BuildMissingValueResult("--udp-jitter-buffer-adaptive-max-ms");
        }
        if (!ParseInt(argv[++(*index)], &config->udp_jitter_buffer_adaptive_max_delay_ms)) {
            return BuildInvalidValueResult("--udp-jitter-buffer-adaptive-max-ms");
        }
        return BuildHandledSuccess();
    }

    // 解析 --udp-jitter-buffer-min-ms <ms>。
    if (std::strcmp(argv[*index], "--udp-jitter-buffer-min-ms") == 0) {
        if (*index + 1 >= argc) {
            return BuildMissingValueResult("--udp-jitter-buffer-min-ms");
        }
        if (!ParseInt(argv[++(*index)], &config->udp_jitter_buffer_min_delay_ms)) {
            return BuildInvalidValueResult("--udp-jitter-buffer-min-ms");
        }
        return BuildHandledSuccess();
    }

    // 解析 --udp-jitter-buffer-safety <factor>
    if (std::strcmp(argv[*index], "--udp-jitter-buffer-safety") == 0) {
        if (*index + 1 >= argc) {
            return BuildMissingValueResult("--udp-jitter-buffer-safety");
        }
        if (!ParseDouble(argv[++(*index)], &config->udp_jitter_buffer_safety_factor) ||
            config->udp_jitter_buffer_safety_factor <= 0.0) {
            return BuildInvalidValueResult("--udp-jitter-buffer-safety");
        }
        return BuildHandledSuccess();
    }

    if (std::strcmp(argv[*index], "--udp-jitter-buffer-max-wait-ms") == 0) {
        if (*index + 1 >= argc) {
            return BuildMissingValueResult("--udp-jitter-buffer-max-wait-ms");
        }
        if (!ParseInt(argv[++(*index)], &config->udp_jitter_buffer_max_wait_ms)) {
            return BuildInvalidValueResult("--udp-jitter-buffer-max-wait-ms");
        }
        return BuildHandledSuccess();
    }

    if (std::strcmp(argv[*index], "--udp-jitter-buffer-max-frames") == 0) {
        if (*index + 1 >= argc) {
            return BuildMissingValueResult("--udp-jitter-buffer-max-frames");
        }
        {
            int parsed = 0;
            if (!ParseInt(argv[++(*index)], &parsed)) {
                return BuildInvalidValueResult("--udp-jitter-buffer-max-frames");
            }
            config->udp_jitter_buffer_max_frames = static_cast<std::size_t>(std::max(1, parsed));
        }
        return BuildHandledSuccess();
    }
    if (std::strcmp(argv[*index], "--udp-nack") == 0) {
        return ParseOnOffOption(argc, argv, index, "--udp-nack", &config->udp_nack_enabled);
    }
    if (std::strcmp(argv[*index], "--udp-fec") == 0) {
        return ParseOnOffOption(argc, argv, index, "--udp-fec", &config->udp_fec_enabled);
    }
    if (std::strcmp(argv[*index], "--udp-nack-delay-ms") == 0) {
        if (*index + 1 >= argc) {
            return BuildMissingValueResult("--udp-nack-delay-ms");
        }
        if (!ParseInt(argv[++(*index)], &config->udp_nack_request_delay_ms)) {
            return BuildInvalidValueResult("--udp-nack-delay-ms");
        }
        return BuildHandledSuccess();
    }
    if (std::strcmp(argv[*index], "--udp-nack-retry-ms") == 0) {
        if (*index + 1 >= argc) {
            return BuildMissingValueResult("--udp-nack-retry-ms");
        }
        if (!ParseInt(argv[++(*index)], &config->udp_nack_retry_interval_ms)) {
            return BuildInvalidValueResult("--udp-nack-retry-ms");
        }
        return BuildHandledSuccess();
    }
    if (std::strcmp(argv[*index], "--udp-nack-max-retries") == 0) {
        if (*index + 1 >= argc) {
            return BuildMissingValueResult("--udp-nack-max-retries");
        }
        if (!ParseInt(argv[++(*index)], &config->udp_nack_max_retries)) {
            return BuildInvalidValueResult("--udp-nack-max-retries");
        }
        return BuildHandledSuccess();
    }
    if (std::strcmp(argv[*index], "--inject-loss-pattern") == 0) {
        if (*index + 1 >= argc) {
            return BuildMissingValueResult("--inject-loss-pattern");
        }
        config->udp_test_loss_pattern = argv[++(*index)];
        if (config->udp_test_loss_pattern != "none" &&
            config->udp_test_loss_pattern != "single" &&
            config->udp_test_loss_pattern != "burst" &&
            config->udp_test_loss_pattern != "alternate") {
            return BuildInvalidValueResult("--inject-loss-pattern");
        }
        return BuildHandledSuccess();
    }
    if (std::strcmp(argv[*index], "--inject-loss-period") == 0) {
        if (*index + 1 >= argc) {
            return BuildMissingValueResult("--inject-loss-period");
        }
        {
            int parsed = 0;
            if (!ParseInt(argv[++(*index)], &parsed)) {
                return BuildInvalidValueResult("--inject-loss-period");
            }
            config->udp_test_loss_period = std::max(1, parsed);
        }
        return BuildHandledSuccess();
    }
    if (std::strcmp(argv[*index], "--inject-loss-count") == 0) {
        if (*index + 1 >= argc) {
            return BuildMissingValueResult("--inject-loss-count");
        }
        {
            int parsed = 0;
            if (!ParseInt(argv[++(*index)], &parsed)) {
                return BuildInvalidValueResult("--inject-loss-count");
            }
            config->udp_test_loss_count = std::max(1, parsed);
        }
        return BuildHandledSuccess();
    }
    if (std::strcmp(argv[*index], "--decoder") == 0) {
        if (*index + 1 >= argc) {
            return BuildMissingValueResult("--decoder");
        }
        if (!ParseDecodeBackend(argv[++(*index)], decode_backend)) {
            return BuildInvalidValueResult("--decoder");
        }
        return BuildHandledSuccess();
    }
    if (std::strcmp(argv[*index], "--help") == 0) {
        return BuildHelpResult();
    }

    return CliParseResult();
}

// 解析普通 sclient 客户端选项
CliParseResult ParseClientOption(
        int argc,
        char **argv,
        int *index,
        ClientConfig *config,
        DecodeBackend *decode_backend,
        RenderBackend *render_backend,
        bool *renderer_vsync_enabled,
        std::string *window_title,
        std::size_t *receive_queue_capacity,
        std::size_t *decode_queue_capacity) {

    // 先尝试解析通用流媒体参数
    CliParseResult shared_result = ParseSharedStreamOption(argc, argv, index, config, decode_backend);

    // 如果通用参数解析器已经处理了这个参数，直接返回
    if (shared_result.handled) {
        return shared_result;
    }

    // 检查普通客户端专属参数需要的指针是否有效
    if (index == nullptr ||
        config == nullptr ||
        render_backend == nullptr ||
        renderer_vsync_enabled == nullptr ||
        window_title == nullptr ||
        receive_queue_capacity == nullptr ||
        decode_queue_capacity == nullptr) {
        CliParseResult result;
        result.handled = true;
        result.success = false;
        result.error_message = "internal cli parser error";
        return result;
    }

    if (std::strcmp(argv[*index], "--transport") == 0) {
        if (*index + 1 >= argc) {
            return BuildMissingValueResult("--transport");
        }
        config->transport = argv[++(*index)];
        return BuildHandledSuccess();
    }
    if (std::strcmp(argv[*index], "--renderer") == 0) {
        if (*index + 1 >= argc) {
            return BuildMissingValueResult("--renderer");
        }
        if (!ParseRenderBackend(argv[++(*index)], render_backend)) {
            return BuildInvalidValueResult("--renderer");
        }
        return BuildHandledSuccess();
    }
    if (std::strcmp(argv[*index], "--vsync") == 0) {
        return ParseOnOffOption(argc, argv, index, "--vsync", renderer_vsync_enabled);
    }
    if (std::strcmp(argv[*index], "--window-title") == 0) {
        if (*index + 1 >= argc) {
            return BuildMissingValueResult("--window-title");
        }
        *window_title = argv[++(*index)];
        return BuildHandledSuccess();
    }
    if (std::strcmp(argv[*index], "--receive-queue") == 0) {
        if (*index + 1 >= argc) {
            return BuildMissingValueResult("--receive-queue");
        }
        int parsed = 0;
        if (!ParseInt(argv[++(*index)], &parsed) || parsed < 1) {
            return BuildInvalidValueResult("--receive-queue");
        }
        *receive_queue_capacity = static_cast<std::size_t>(parsed);
        return BuildHandledSuccess();
    }
    if (std::strcmp(argv[*index], "--decode-queue") == 0) {
        if (*index + 1 >= argc) {
            return BuildMissingValueResult("--decode-queue");
        }
        int parsed = 0;
        if (!ParseInt(argv[++(*index)], &parsed) || parsed < 1) {
            return BuildInvalidValueResult("--decode-queue");
        }
        *decode_queue_capacity = static_cast<std::size_t>(parsed);
        return BuildHandledSuccess();
    }

    return CliParseResult();
}

// 解析 UDP benchmark 程序选项。
CliParseResult ParseUdpBenchmarkOption(
        int argc,
        char **argv,
        int *index,
        ClientConfig *config,
        DecodeBackend *decode_backend,
        int *frames_to_measure,
        bool *decode_frames) {
    CliParseResult shared_result = ParseSharedStreamOption(argc, argv, index, config, decode_backend);
    if (shared_result.handled) {
        return shared_result;
    }

    if (index == nullptr || config == nullptr || frames_to_measure == nullptr || decode_frames == nullptr) {
        CliParseResult result;
        result.handled = true;
        result.success = false;
        result.error_message = "internal cli parser error";
        return result;
    }

    if (std::strcmp(argv[*index], "--frames") == 0) {
        if (*index + 1 >= argc) {
            return BuildMissingValueResult("--frames");
        }
        {
            int parsed = 0;
            if (!ParseInt(argv[++(*index)], &parsed)) {
                return BuildInvalidValueResult("--frames");
            }
            *frames_to_measure = std::max(1, parsed);
        }
        return BuildHandledSuccess();
    }
    if (std::strcmp(argv[*index], "--decode") == 0) {
        return ParseOnOffOption(argc, argv, index, "--decode", decode_frames);
    }
    if (std::strcmp(argv[*index], "--inject-jitter-pattern") == 0) {
        if (*index + 1 >= argc) {
            return BuildMissingValueResult("--inject-jitter-pattern");
        }
        config->udp_test_jitter_pattern = argv[++(*index)];
        if (config->udp_test_jitter_pattern != "none" &&
            config->udp_test_jitter_pattern != "saw" &&
            config->udp_test_jitter_pattern != "burst" &&
            config->udp_test_jitter_pattern != "alternate") {
            return BuildInvalidValueResult("--inject-jitter-pattern");
        }
        return BuildHandledSuccess();
    }
    if (std::strcmp(argv[*index], "--inject-jitter-amplitude-ms") == 0) {
        if (*index + 1 >= argc) {
            return BuildMissingValueResult("--inject-jitter-amplitude-ms");
        }
        if (!ParseInt(argv[++(*index)], &config->udp_test_jitter_amplitude_ms)) {
            return BuildInvalidValueResult("--inject-jitter-amplitude-ms");
        }
        return BuildHandledSuccess();
    }
    if (std::strcmp(argv[*index], "--inject-jitter-period") == 0) {
        if (*index + 1 >= argc) {
            return BuildMissingValueResult("--inject-jitter-period");
        }
        {
            int parsed = 0;
            if (!ParseInt(argv[++(*index)], &parsed)) {
                return BuildInvalidValueResult("--inject-jitter-period");
            }
            config->udp_test_jitter_period = std::max(2, parsed);
        }
        return BuildHandledSuccess();
    }

    return CliParseResult();
}

}  // namespace sclient
