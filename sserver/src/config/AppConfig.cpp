#include "config/AppConfig.h"

#include "common/net/RtpProtocol.h"
#include "modules/capture/video/Capture.h"
#include "modules/encoding/video/VideoEncoder.h"
#include "modules/transport/Transport.h"

#include <algorithm>
#include <cctype>
#include <fstream>
#include <sstream>
#include <unordered_map>

namespace sserver {
namespace config {

namespace {

// 去掉字符串首尾空白。
// 配置文件里允许写成 "key = value"，因此解析 key/value 前都需要 Trim。
std::string Trim(const std::string &value) {
    std::size_t start = 0;
    while (start < value.size() && std::isspace(static_cast<unsigned char>(value[start])) != 0) {
        ++start;
    }

    std::size_t end = value.size();
    while (end > start && std::isspace(static_cast<unsigned char>(value[end - 1])) != 0) {
        --end;
    }

    return value.substr(start, end - start);
}

// 解析布尔值。
// 这里特意支持 true/false、1/0、yes/no、on/off，方便配置文件更易读。
bool ParseBool(const std::string &value, bool *result) {
    const std::string normalized = Trim(value);
    if (normalized == "true" || normalized == "1" || normalized == "yes" || normalized == "on") {
        *result = true;
        return true;
    }
    if (normalized == "false" || normalized == "0" || normalized == "no" || normalized == "off") {
        *result = false;
        return true;
    }
    return false;
}

// 解析 int 类型配置，并要求整段 value 都被消费。
// 例如 "30" 合法，"30fps" 会失败。
bool ParseInt(const std::string &value, int *result) {
    std::istringstream stream(Trim(value));
    stream >> *result;
    return !stream.fail() && stream.eof();
}

// 解析 size_t 类型配置，通常用于字节数、队列长度、分片大小等非负数量。
bool ParseSize(const std::string &value, std::size_t *result) {
    std::istringstream stream(Trim(value));
    stream >> *result;
    return !stream.fail() && stream.eof();
}

}  // namespace

AppConfig AppConfig::CreateDefault() {
    // 利用结构体成员默认值生成一份完整默认配置。
    // 后续 LoadFromFile 只覆盖文件中出现的 key。
    return AppConfig();
}

bool AppConfig::Validate(std::string *error_message) const {
    // app.name 主要用于日志和场景识别，不能为空。
    if (app_name.empty()) {
        if (error_message != nullptr) {
            *error_message = "app.name must not be empty";
        }
        return false;
    }

    // runtime 配置只要求数值范围合理。
    if (runtime.shutdown_grace_period_ms < 0) {
        if (error_message != nullptr) {
            *error_message = "runtime.shutdown_grace_period_ms must be >= 0";
        }
        return false;
    }
    if (runtime.latency_log_interval_frames <= 0) {
        if (error_message != nullptr) {
            *error_message = "runtime.latency_log_interval_frames must be > 0";
        }
        return false;
    }

    if (capture.enabled) {
        // 先让 capture 模块自己的解析函数判断 source 是否支持。
        // 这样 AppConfig 不需要硬编码所有 capture backend 细节。
        modules::capture::CaptureBackendSelection selection;
        if (!modules::capture::ResolveCaptureBackendSelection(capture, &selection, error_message)) {
            return false;
        }
        // 采集宽高、fps 必须有效；frame_interval_ms 可以为 0，但不能为负。
        if (capture.width <= 0 || capture.height <= 0 || capture.fps <= 0 || capture.frame_interval_ms < 0) {
            if (error_message != nullptr) {
                *error_message = "capture dimensions, fps and frame_interval_ms must be positive";
            }
            return false;
        }
        // V4L2 至少需要一个设备缓冲区。实际低延迟场景通常用较少缓冲。
        if (capture.device_buffer_count <= 0) {
            if (error_message != nullptr) {
                *error_message = "capture.device_buffer_count must be > 0";
            }
            return false;
        }
        // null 源只允许两种模式，避免拼写错误悄悄落入未知行为。
        if (capture.null_payload_mode != "text" &&
            capture.null_payload_mode != "h264_test_pattern") {
            if (error_message != nullptr) {
                *error_message = "capture.null_payload_mode must be 'text' or 'h264_test_pattern'";
            }
            return false;
        }
    }

    if (transport.enabled) {
        // 让 transport 模块判断 backend 是否存在，例如 tcp/udp/rtp。
        modules::transport::TransportBackendSelection selection;
        if (!modules::transport::ResolveTransportBackendSelection(transport, &selection, error_message)) {
            return false;
        }
        // 端口 0 合法，表示由系统自动选择；1-65535 是普通有效端口。
        if (transport.listen_port < 0 || transport.listen_port > 65535) {
            if (error_message != nullptr) {
                *error_message = "transport.listen_port must be in [0, 65535]";
            }
            return false;
        }
        // 发送队列至少能容纳一帧。低延迟场景通常设置得很小。
        if (transport.max_pending_frames == 0) {
            if (error_message != nullptr) {
                *error_message = "transport.max_pending_frames must be > 0";
            }
            return false;
        }
        // 等待时间不能为负。
        if (transport.max_queue_wait_ms < 0) {
            if (error_message != nullptr) {
                *error_message = "transport.max_queue_wait_ms must be >= 0";
            }
            return false;
        }
        // 丢帧策略必须是传输模块明确支持的策略。
        if (transport.queue_drop_policy != "drop_oldest" &&
            transport.queue_drop_policy != "drop_oldest_non_key") {
            if (error_message != nullptr) {
                *error_message = "transport.queue_drop_policy must be 'drop_oldest' or 'drop_oldest_non_key'";
            }
            return false;
        }
        // accept 循环间隔不能为负。0 表示尽可能快地轮询。
        if (transport.accept_loop_interval_ms < 0) {
            if (error_message != nullptr) {
                *error_message = "transport.accept_loop_interval_ms must be >= 0";
            }
            return false;
        }
        // UDP 客户端依靠 keepalive 注册，超时时间必须大于 0。
        if (transport.udp_client_timeout_ms <= 0) {
            if (error_message != nullptr) {
                *error_message = "transport.udp_client_timeout_ms must be > 0";
            }
            return false;
        }
        // UDP payload 理论上限是 65507。下限 512 用来避免配置过小导致效率很差。
        if (transport.udp_max_datagram_size < 512 || transport.udp_max_datagram_size > 65507) {
            if (error_message != nullptr) {
                *error_message = "transport.udp_max_datagram_size must be in [512, 65507]";
            }
            return false;
        }
        // 目标分片大小不能超过 datagram 上限，也不能太小。
        if (transport.udp_target_payload_size < 256 || transport.udp_target_payload_size > transport.udp_max_datagram_size) {
            if (error_message != nullptr) {
                *error_message = "transport.udp_target_payload_size must be in [256, transport.udp_max_datagram_size]";
            }
            return false;
        }
        // socket 缓冲区必须大于 0。
        if (transport.udp_receive_buffer_bytes <= 0 || transport.udp_send_buffer_bytes <= 0) {
            if (error_message != nullptr) {
                *error_message = "transport UDP buffer sizes must be > 0";
            }
            return false;
        }
        // NACK 重传需要缓存历史帧，缓存帧数不能为 0。
        if (transport.udp_retransmit_cache_frames == 0) {
            if (error_message != nullptr) {
                *error_message = "transport.udp_retransmit_cache_frames must be > 0";
            }
            return false;
        }
        // 缓存老化时间必须为正，否则刚放入缓存就可能失效。
        if (transport.udp_retransmit_cache_max_age_ms <= 0) {
            if (error_message != nullptr) {
                *error_message = "transport.udp_retransmit_cache_max_age_ms must be > 0";
            }
            return false;
        }
        // 限制单次 NACK 重传数量，避免一次请求导致发送突发过大。
        if (transport.udp_retransmit_max_fragments_per_request == 0) {
            if (error_message != nullptr) {
                *error_message = "transport.udp_retransmit_max_fragments_per_request must be > 0";
            }
            return false;
        }
        if (transport.backend == "rtp") {
            // RTP 在 UDP payload 里还要额外放 RTP 头和可选延迟扩展头，
            // 所以真正可用于 H.264 的最大 payload 要扣掉协议开销。
            const std::size_t max_rtp_payload_size =
                    transport.udp_max_datagram_size > common::net::kRtpPacketOverheadWithLatencyExtension
                            ? transport.udp_max_datagram_size - common::net::kRtpPacketOverheadWithLatencyExtension
                            : 0;
            // RTP 是服务端主动推送到远端端口，远端端口不能为 0。
            if (transport.rtp_remote_port <= 0 || transport.rtp_remote_port > 65535) {
                if (error_message != nullptr) {
                    *error_message = "transport.rtp_remote_port must be in [1, 65535]";
                }
                return false;
            }
            // RTP payload type 是 7 bit 字段，范围 0-127。
            if (transport.rtp_payload_type < 0 || transport.rtp_payload_type > 127) {
                if (error_message != nullptr) {
                    *error_message = "transport.rtp_payload_type must be in [0, 127]";
                }
                return false;
            }
            // H.264 RTP 通常是 90000 Hz，但这里仅要求为正。
            if (transport.rtp_clock_rate <= 0) {
                if (error_message != nullptr) {
                    *error_message = "transport.rtp_clock_rate must be > 0";
                }
                return false;
            }
            // SSRC 这里用 int 存储，因此至少不能为负。
            if (transport.rtp_ssrc < 0) {
                if (error_message != nullptr) {
                    *error_message = "transport.rtp_ssrc must be >= 0";
                }
                return false;
            }
            // RTP payload 太小会让分片过多；太大会超过 UDP 可承载范围。
            if (transport.rtp_max_payload_size < 64 || transport.rtp_max_payload_size > max_rtp_payload_size) {
                if (error_message != nullptr) {
                    *error_message =
                            "transport.rtp_max_payload_size must be in [64, transport.udp_max_datagram_size - RTP header overhead]";
                }
                return false;
            }
            // RTP 远端地址不能为空。
            if (transport.rtp_remote_host.empty()) {
                if (error_message != nullptr) {
                    *error_message = "transport.rtp_remote_host must not be empty";
                }
                return false;
            }
        }
    }

    // 编码器后端同样交给 encoding 模块解析，避免 AppConfig 硬编码后端枚举。
    modules::encoding::VideoEncoderBackendSelection selection;
    if (!modules::encoding::ResolveVideoEncoderBackendSelection(codec, &selection, error_message)) {
        return false;
    }
    if (selection.backend == modules::encoding::EncodeBackend::kX264) {
        // x264 的这些整数配置必须为正，否则编码器无法合理初始化。
        if (codec.x264_keyint_max <= 0 || codec.x264_keyint_min <= 0 || codec.x264_threads <= 0 ||
            codec.x264_slice_count <= 0 || codec.x264_slice_count_max <= 0 || codec.x264_frame_reference <= 0) {
            if (error_message != nullptr) {
                *error_message = "codec x264 integer settings must be positive";
            }
            return false;
        }
        // 这些配置允许为 0，但不能为负。
        // 对低延迟来说，bframes/lookahead 通常就设置为 0。
        if (codec.x264_bframes < 0 || codec.x264_lookahead < 0 || codec.x264_sync_lookahead < 0 ||
            codec.x264_subpel_refine < 0) {
            if (error_message != nullptr) {
                *error_message = "codec x264 lookahead/bframes/subpel settings must be >= 0";
            }
            return false;
        }
    }

    return true;
}

bool ConfigLoader::LoadFromFile(const std::string &file_path, AppConfig *config, std::string *error_message) {
    // 调用方必须提供输出对象。
    if (config == nullptr) {
        if (error_message != nullptr) {
            *error_message = "config output pointer is null";
        }
        return false;
    }

    // 打开配置文件。这里使用传入路径本身，不做相对路径修正。
    // 所以 main.cpp 从哪个目录运行，会影响 "config/xxx.conf" 的解析位置。
    std::ifstream input(file_path.c_str());
    if (!input.is_open()) {
        if (error_message != nullptr) {
            *error_message = "failed to open config file: " + file_path;
        }
        return false;
    }

    // 先创建默认配置，再逐行用配置文件覆盖。
    // 好处：配置文件不必写出所有 key；坏处：拼错 key 会被视为不支持并报错。
    AppConfig loaded = AppConfig::CreateDefault();
    std::string line;
    int line_number = 0;

    while (std::getline(input, line)) {
        ++line_number;
        const std::string trimmed = Trim(line);
        // 跳过空行和整行注释。
        if (trimmed.empty() || trimmed[0] == '#') {
            continue;
        }

        // 当前配置格式只支持 key = value。缺少 '=' 直接报错。
        const std::size_t equal_pos = trimmed.find('=');
        if (equal_pos == std::string::npos) {
            if (error_message != nullptr) {
                *error_message = "invalid config line " + std::to_string(line_number) + ": missing '='";
            }
            return false;
        }

        // 分别修剪 key 和 value 两侧空白。
        // 例如 "capture.width = 640" 会得到 key="capture.width", value="640"。
        const std::string key = Trim(trimmed.substr(0, equal_pos));
        const std::string value = Trim(trimmed.substr(equal_pos + 1));

        // 下面是一张显式 key -> AppConfig 字段的映射表。
        // 优点是直观、错误信息容易定位；缺点是新增配置项时需要在这里补分支。
        if (key == "app.name") {
            loaded.app_name = value;
        } else if (key == "runtime.shutdown_grace_period_ms") {
            if (!ParseInt(value, &loaded.runtime.shutdown_grace_period_ms)) {
                if (error_message != nullptr) {
                    *error_message = "invalid integer at line " + std::to_string(line_number);
                }
                return false;
            }
        } else if (key == "runtime.latency_log_interval_frames") {
            if (!ParseInt(value, &loaded.runtime.latency_log_interval_frames)) {
                if (error_message != nullptr) {
                    *error_message = "invalid runtime.latency_log_interval_frames at line " + std::to_string(line_number);
                }
                return false;
            }
        } else if (key == "capture.enabled") {
            if (!ParseBool(value, &loaded.capture.enabled)) {
                if (error_message != nullptr) {
                    *error_message = "invalid boolean at line " + std::to_string(line_number);
                }
                return false;
            }
        } else if (key == "capture.source") {
            loaded.capture.source = value;
        } else if (key == "capture.device") {
            loaded.capture.device = value;
        } else if (key == "capture.width") {
            if (!ParseInt(value, &loaded.capture.width)) {
                if (error_message != nullptr) {
                    *error_message = "invalid capture.width at line " + std::to_string(line_number);
                }
                return false;
            }
        } else if (key == "capture.height") {
            if (!ParseInt(value, &loaded.capture.height)) {
                if (error_message != nullptr) {
                    *error_message = "invalid capture.height at line " + std::to_string(line_number);
                }
                return false;
            }
        } else if (key == "capture.fps") {
            if (!ParseInt(value, &loaded.capture.fps)) {
                if (error_message != nullptr) {
                    *error_message = "invalid capture.fps at line " + std::to_string(line_number);
                }
                return false;
            }
        } else if (key == "capture.frame_interval_ms") {
            if (!ParseInt(value, &loaded.capture.frame_interval_ms)) {
                if (error_message != nullptr) {
                    *error_message = "invalid capture.frame_interval_ms at line " + std::to_string(line_number);
                }
                return false;
            }
        } else if (key == "capture.device_buffer_count") {
            if (!ParseInt(value, &loaded.capture.device_buffer_count)) {
                if (error_message != nullptr) {
                    *error_message = "invalid capture.device_buffer_count at line " + std::to_string(line_number);
                }
                return false;
            }
        } else if (key == "capture.null_payload_bytes") {
            if (!ParseSize(value, &loaded.capture.null_payload_bytes)) {
                if (error_message != nullptr) {
                    *error_message = "invalid capture.null_payload_bytes at line " + std::to_string(line_number);
                }
                return false;
            }
        } else if (key == "capture.null_payload_mode") {
            loaded.capture.null_payload_mode = value;
        } else if (key == "transport.enabled") {
            if (!ParseBool(value, &loaded.transport.enabled)) {
                if (error_message != nullptr) {
                    *error_message = "invalid transport.enabled at line " + std::to_string(line_number);
                }
                return false;
            }
        } else if (key == "transport.backend") {
            loaded.transport.backend = value;
        } else if (key == "transport.bind_address") {
            loaded.transport.bind_address = value;
        } else if (key == "transport.listen_port") {
            if (!ParseInt(value, &loaded.transport.listen_port)) {
                if (error_message != nullptr) {
                    *error_message = "invalid transport.listen_port at line " + std::to_string(line_number);
                }
                return false;
            }
        } else if (key == "transport.max_pending_frames") {
            if (!ParseSize(value, &loaded.transport.max_pending_frames)) {
                if (error_message != nullptr) {
                    *error_message = "invalid transport.max_pending_frames at line " + std::to_string(line_number);
                }
                return false;
            }
        } else if (key == "transport.max_queue_wait_ms") {
            if (!ParseInt(value, &loaded.transport.max_queue_wait_ms)) {
                if (error_message != nullptr) {
                    *error_message = "invalid transport.max_queue_wait_ms at line " + std::to_string(line_number);
                }
                return false;
            }
        } else if (key == "transport.queue_drop_policy") {
            loaded.transport.queue_drop_policy = value;
        } else if (key == "transport.accept_loop_interval_ms") {
            if (!ParseInt(value, &loaded.transport.accept_loop_interval_ms)) {
                if (error_message != nullptr) {
                    *error_message = "invalid transport.accept_loop_interval_ms at line " + std::to_string(line_number);
                }
                return false;
            }
        } else if (key == "transport.enable_nodelay") {
            if (!ParseBool(value, &loaded.transport.enable_nodelay)) {
                if (error_message != nullptr) {
                    *error_message = "invalid transport.enable_nodelay at line " + std::to_string(line_number);
                }
                return false;
            }
        } else if (key == "transport.embed_frame_metadata") {
            if (!ParseBool(value, &loaded.transport.embed_frame_metadata)) {
                if (error_message != nullptr) {
                    *error_message = "invalid transport.embed_frame_metadata at line " + std::to_string(line_number);
                }
                return false;
            }
        } else if (key == "transport.udp_client_timeout_ms") {
            if (!ParseInt(value, &loaded.transport.udp_client_timeout_ms)) {
                if (error_message != nullptr) {
                    *error_message = "invalid transport.udp_client_timeout_ms at line " + std::to_string(line_number);
                }
                return false;
            }
        } else if (key == "transport.udp_max_datagram_size") {
            if (!ParseSize(value, &loaded.transport.udp_max_datagram_size)) {
                if (error_message != nullptr) {
                    *error_message = "invalid transport.udp_max_datagram_size at line " + std::to_string(line_number);
                }
                return false;
            }
        } else if (key == "transport.udp_target_payload_size") {
            if (!ParseSize(value, &loaded.transport.udp_target_payload_size)) {
                if (error_message != nullptr) {
                    *error_message = "invalid transport.udp_target_payload_size at line " + std::to_string(line_number);
                }
                return false;
            }
        } else if (key == "transport.udp_receive_buffer_bytes") {
            if (!ParseInt(value, &loaded.transport.udp_receive_buffer_bytes)) {
                if (error_message != nullptr) {
                    *error_message = "invalid transport.udp_receive_buffer_bytes at line " + std::to_string(line_number);
                }
                return false;
            }
        } else if (key == "transport.udp_send_buffer_bytes") {
            if (!ParseInt(value, &loaded.transport.udp_send_buffer_bytes)) {
                if (error_message != nullptr) {
                    *error_message = "invalid transport.udp_send_buffer_bytes at line " + std::to_string(line_number);
                }
                return false;
            }
        } else if (key == "transport.udp_enable_nack") {
            if (!ParseBool(value, &loaded.transport.udp_enable_nack)) {
                if (error_message != nullptr) {
                    *error_message = "invalid transport.udp_enable_nack at line " + std::to_string(line_number);
                }
                return false;
            }
        } else if (key == "transport.udp_enable_fec") {
            if (!ParseBool(value, &loaded.transport.udp_enable_fec)) {
                if (error_message != nullptr) {
                    *error_message = "invalid transport.udp_enable_fec at line " + std::to_string(line_number);
                }
                return false;
            }
        } else if (key == "transport.udp_retransmit_cache_frames") {
            if (!ParseSize(value, &loaded.transport.udp_retransmit_cache_frames)) {
                if (error_message != nullptr) {
                    *error_message = "invalid transport.udp_retransmit_cache_frames at line " + std::to_string(line_number);
                }
                return false;
            }
        } else if (key == "transport.udp_retransmit_cache_max_age_ms") {
            if (!ParseInt(value, &loaded.transport.udp_retransmit_cache_max_age_ms)) {
                if (error_message != nullptr) {
                    *error_message = "invalid transport.udp_retransmit_cache_max_age_ms at line " + std::to_string(line_number);
                }
                return false;
            }
        } else if (key == "transport.udp_retransmit_max_fragments_per_request") {
            if (!ParseSize(value, &loaded.transport.udp_retransmit_max_fragments_per_request)) {
                if (error_message != nullptr) {
                    *error_message = "invalid transport.udp_retransmit_max_fragments_per_request at line " + std::to_string(line_number);
                }
                return false;
            }
        } else if (key == "transport.rtp_remote_host") {
            loaded.transport.rtp_remote_host = value;
        } else if (key == "transport.rtp_remote_port") {
            if (!ParseInt(value, &loaded.transport.rtp_remote_port)) {
                if (error_message != nullptr) {
                    *error_message = "invalid transport.rtp_remote_port at line " + std::to_string(line_number);
                }
                return false;
            }
        } else if (key == "transport.rtp_payload_type") {
            if (!ParseInt(value, &loaded.transport.rtp_payload_type)) {
                if (error_message != nullptr) {
                    *error_message = "invalid transport.rtp_payload_type at line " + std::to_string(line_number);
                }
                return false;
            }
        } else if (key == "transport.rtp_clock_rate") {
            if (!ParseInt(value, &loaded.transport.rtp_clock_rate)) {
                if (error_message != nullptr) {
                    *error_message = "invalid transport.rtp_clock_rate at line " + std::to_string(line_number);
                }
                return false;
            }
        } else if (key == "transport.rtp_ssrc") {
            if (!ParseInt(value, &loaded.transport.rtp_ssrc)) {
                if (error_message != nullptr) {
                    *error_message = "invalid transport.rtp_ssrc at line " + std::to_string(line_number);
                }
                return false;
            }
        } else if (key == "transport.rtp_max_payload_size") {
            if (!ParseSize(value, &loaded.transport.rtp_max_payload_size)) {
                if (error_message != nullptr) {
                    *error_message = "invalid transport.rtp_max_payload_size at line " + std::to_string(line_number);
                }
                return false;
            }
        } else if (key == "transport.rtp_enable_latency_extension") {
            if (!ParseBool(value, &loaded.transport.rtp_enable_latency_extension)) {
                if (error_message != nullptr) {
                    *error_message =
                            "invalid transport.rtp_enable_latency_extension at line " + std::to_string(line_number);
                }
                return false;
            }
        } else if (key == "transport.rtp_sdp_path") {
            loaded.transport.rtp_sdp_path = value;
        } else if (key == "codec.backend") {
            loaded.codec.backend = value;
        } else if (key == "codec.x264_preset") {
            loaded.codec.x264_preset = value;
        } else if (key == "codec.x264_tune") {
            loaded.codec.x264_tune = value;
        } else if (key == "codec.x264_profile") {
            loaded.codec.x264_profile = value;
        } else if (key == "codec.x264_annexb") {
            if (!ParseBool(value, &loaded.codec.x264_annexb)) {
                if (error_message != nullptr) {
                    *error_message = "invalid codec.x264_annexb at line " + std::to_string(line_number);
                }
                return false;
            }
        } else if (key == "codec.x264_repeat_headers") {
            if (!ParseBool(value, &loaded.codec.x264_repeat_headers)) {
                if (error_message != nullptr) {
                    *error_message = "invalid codec.x264_repeat_headers at line " + std::to_string(line_number);
                }
                return false;
            }
        } else if (key == "codec.x264_keyint_max") {
            if (!ParseInt(value, &loaded.codec.x264_keyint_max)) {
                if (error_message != nullptr) {
                    *error_message = "invalid codec.x264_keyint_max at line " + std::to_string(line_number);
                }
                return false;
            }
        } else if (key == "codec.x264_keyint_min") {
            if (!ParseInt(value, &loaded.codec.x264_keyint_min)) {
                if (error_message != nullptr) {
                    *error_message = "invalid codec.x264_keyint_min at line " + std::to_string(line_number);
                }
                return false;
            }
        } else if (key == "codec.x264_bframes") {
            if (!ParseInt(value, &loaded.codec.x264_bframes)) {
                if (error_message != nullptr) {
                    *error_message = "invalid codec.x264_bframes at line " + std::to_string(line_number);
                }
                return false;
            }
        } else if (key == "codec.x264_threads") {
            if (!ParseInt(value, &loaded.codec.x264_threads)) {
                if (error_message != nullptr) {
                    *error_message = "invalid codec.x264_threads at line " + std::to_string(line_number);
                }
                return false;
            }
        } else if (key == "codec.x264_sliced_threads") {
            if (!ParseBool(value, &loaded.codec.x264_sliced_threads)) {
                if (error_message != nullptr) {
                    *error_message = "invalid codec.x264_sliced_threads at line " + std::to_string(line_number);
                }
                return false;
            }
        } else if (key == "codec.x264_slice_count") {
            if (!ParseInt(value, &loaded.codec.x264_slice_count)) {
                if (error_message != nullptr) {
                    *error_message = "invalid codec.x264_slice_count at line " + std::to_string(line_number);
                }
                return false;
            }
        } else if (key == "codec.x264_slice_count_max") {
            if (!ParseInt(value, &loaded.codec.x264_slice_count_max)) {
                if (error_message != nullptr) {
                    *error_message = "invalid codec.x264_slice_count_max at line " + std::to_string(line_number);
                }
                return false;
            }
        } else if (key == "codec.x264_frame_reference") {
            if (!ParseInt(value, &loaded.codec.x264_frame_reference)) {
                if (error_message != nullptr) {
                    *error_message = "invalid codec.x264_frame_reference at line " + std::to_string(line_number);
                }
                return false;
            }
        } else if (key == "codec.x264_lookahead") {
            if (!ParseInt(value, &loaded.codec.x264_lookahead)) {
                if (error_message != nullptr) {
                    *error_message = "invalid codec.x264_lookahead at line " + std::to_string(line_number);
                }
                return false;
            }
        } else if (key == "codec.x264_sync_lookahead") {
            if (!ParseInt(value, &loaded.codec.x264_sync_lookahead)) {
                if (error_message != nullptr) {
                    *error_message = "invalid codec.x264_sync_lookahead at line " + std::to_string(line_number);
                }
                return false;
            }
        } else if (key == "codec.x264_subpel_refine") {
            if (!ParseInt(value, &loaded.codec.x264_subpel_refine)) {
                if (error_message != nullptr) {
                    *error_message = "invalid codec.x264_subpel_refine at line " + std::to_string(line_number);
                }
                return false;
            }
        } else if (key == "codec.x264_mb_tree") {
            if (!ParseBool(value, &loaded.codec.x264_mb_tree)) {
                if (error_message != nullptr) {
                    *error_message = "invalid codec.x264_mb_tree at line " + std::to_string(line_number);
                }
                return false;
            }
        } else if (key == "codec.x264_scenecut") {
            if (!ParseInt(value, &loaded.codec.x264_scenecut)) {
                if (error_message != nullptr) {
                    *error_message = "invalid codec.x264_scenecut at line " + std::to_string(line_number);
                }
                return false;
            }
        } else {
            // 不允许未知 key 静默通过。
            // 这样配置文件拼写错误会在启动时立刻暴露，而不是使用默认值继续运行。
            if (error_message != nullptr) {
                *error_message = "unsupported config key at line " + std::to_string(line_number) + ": " + key;
            }
            return false;
        }
    }

    // 所有 key 都解析完成后，再做跨字段和范围校验。
    if (!loaded.Validate(error_message)) {
        return false;
    }

    // 只有完整解析并通过校验后，才把结果写回调用方。
    // 这样失败时不会污染调用方已有配置对象。
    *config = loaded;
    return true;
}

}  // namespace config
}  // namespace sserver
