#ifndef SCLIENT_COMMON_CLIOPTIONS_H
#define SCLIENT_COMMON_CLIOPTIONS_H

#include <string>

#include "modules/decoding/VideoDecoder.h"
#include "modules/network/types/ClientConfig.h"
#include "modules/rendering/VideoRenderer.h"

namespace sclient {

/** 命令行解析结果 */
struct CliParseResult {
    bool handled = false;      /**< 是否已处理该参数 */
    bool success = true;       /**< 解析是否成功 */
    bool show_help = false;    /**< 是否显示帮助信息 */
    std::string error_message; /**< 错误信息（解析失败时） */
};

/** 解析布尔标志参数（如 --verbose） */
bool ParseBoolFlag(const char *value, bool *result);
/** 解码后端类型参数 */
bool ParseDecodeBackend(const char *value, DecodeBackend *result);
/** 渲染后端类型参数 */
bool ParseRenderBackend(const char *value, RenderBackend *result);

/** 打印客户端程序使用说明 */
void PrintClientUsage(const char *program_name);
/** 打印 UDP 基准测试使用说明 */
void PrintUdpBenchmarkUsage(const char *program_name);

/**
 * 解析流媒体通用选项
 *
 * 处理客户端和基准测试共用的命令行参数，如服务器地址、端口、协议等
 */
CliParseResult ParseSharedStreamOption(
        int argc,
        char **argv,
        int *index,
        ClientConfig *config,
        DecodeBackend *decode_backend);

/** 解析客户端特有选项（渲染、队列容量等） */
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
        std::size_t *decode_queue_capacity);

/** 解析 UDP 基准测试选项 */
CliParseResult ParseUdpBenchmarkOption(
        int argc,
        char **argv,
        int *index,
        ClientConfig *config,
        DecodeBackend *decode_backend,
        int *frames_to_measure,
        bool *decode_frames);

}  // namespace sclient

#endif  // SCLIENT_COMMON_CLIOPTIONS_H
