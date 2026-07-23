#include "common/net/SdpSessionDescription.h"

#include <cctype>
#include <fstream>
#include <sstream>

namespace sclient {
namespace common {
namespace net {

namespace {

/** 去除字符串首尾空白字符 */
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

/** 安全地将字符串转换为整数 */
bool ParseInt(const std::string &value, int *result) {
    if (result == nullptr) {
        return false;
    }

    std::istringstream stream(value);
    stream >> *result;
    return !stream.fail() && stream.eof();
}

}  // namespace

/**
 * 解析 SDP 内容
 *
 * SDP 格式示例：
 *   v=0
 *   c=IN IP4 192.168.1.100
 *   m=video 5004 RTP/AVP 96
 *   a=rtpmap:96 H264/90000
 *
 * 本函数提取视频流的连接地址、端口、载荷类型和时钟频率
 */
bool ParseRtpVideoSessionDescription(
        const std::string &contents,
        RtpVideoSessionDescription *description,
        std::string *error_message) {
    if (description == nullptr) {
        if (error_message != nullptr) {
            *error_message = "sdp description output is null";
        }
        return false;
    }

    *description = RtpVideoSessionDescription();

    std::istringstream stream(contents);
    std::string line;
    bool have_connection_address = false;
    bool have_media_description = false;
    bool have_rtpmap = false;
    while (std::getline(stream, line)) {
        const std::string trimmed = Trim(line);
        if (trimmed.empty()) {
            continue;
        }

        // 解析连接行：c=IN IP4 <地址>
        if (trimmed.compare(0, 9, "c=IN IP4 ") == 0) {
            description->connection_address = Trim(trimmed.substr(9));
            have_connection_address = !description->connection_address.empty();
            continue;
        }

        // 解析媒体行：m=video <端口> RTP/AVP <载荷类型>
        if (trimmed.compare(0, 8, "m=video ") == 0) {
            std::istringstream media_stream(trimmed.substr(8));
            std::string transport;
            int port = 0;
            int payload_type = -1;
            if (!(media_stream >> port >> transport >> payload_type) ||
                transport != "RTP/AVP" ||
                port <= 0 ||
                payload_type < 0 ||
                payload_type > 127) {
                if (error_message != nullptr) {
                    *error_message = "invalid video media description in SDP";
                }
                return false;
            }

            description->video_port = port;
            description->payload_type = payload_type;
            have_media_description = true;
            continue;
        }

        // 解析 rtpmap 行：a=rtpmap:<载荷类型> <编码>/<时钟频率>
        if (trimmed.compare(0, 9, "a=rtpmap:") == 0) {
            const std::size_t separator = trimmed.find(' ');
            if (separator == std::string::npos) {
                if (error_message != nullptr) {
                    *error_message = "invalid rtpmap line in SDP";
                }
                return false;
            }

            const std::string payload_type_text = trimmed.substr(9, separator - 9);
            int payload_type = -1;
            if (!ParseInt(payload_type_text, &payload_type)) {
                if (error_message != nullptr) {
                    *error_message = "invalid payload type in SDP rtpmap";
                }
                return false;
            }

            const std::string encoding = trimmed.substr(separator + 1);
            // 只处理 H264 编码，忽略其他编码格式
            if (encoding.compare(0, 5, "H264/") != 0) {
                continue;
            }

            int clock_rate = 0;
            if (!ParseInt(encoding.substr(5), &clock_rate) || clock_rate <= 0) {
                if (error_message != nullptr) {
                    *error_message = "invalid H264 clock rate in SDP";
                }
                return false;
            }

            // 校验载荷类型是否与媒体行一致
            if (description->payload_type >= 0 && payload_type != description->payload_type) {
                if (error_message != nullptr) {
                    *error_message = "SDP rtpmap payload type does not match video media description";
                }
                return false;
            }

            description->payload_type = payload_type;
            description->clock_rate = clock_rate;
            have_rtpmap = true;
        }
    }

    // 验证必要字段是否都已解析
    if (!have_connection_address) {
        if (error_message != nullptr) {
            *error_message = "SDP is missing connection address";
        }
        return false;
    }
    if (!have_media_description) {
        if (error_message != nullptr) {
            *error_message = "SDP is missing video media description";
        }
        return false;
    }
    if (!have_rtpmap) {
        if (error_message != nullptr) {
            *error_message = "SDP is missing H264 rtpmap";
        }
        return false;
    }
    return true;
}

bool LoadRtpVideoSessionDescription(
        const std::string &file_path,
        RtpVideoSessionDescription *description,
        std::string *error_message) {
    std::ifstream input(file_path.c_str(), std::ios::in);
    if (!input.is_open()) {
        if (error_message != nullptr) {
            *error_message = "failed to open SDP file: " + file_path;
        }
        return false;
    }

    const std::string contents((std::istreambuf_iterator<char>(input)), std::istreambuf_iterator<char>());
    return ParseRtpVideoSessionDescription(contents, description, error_message);
}

}  // namespace net
}  // namespace common
}  // namespace sclient
