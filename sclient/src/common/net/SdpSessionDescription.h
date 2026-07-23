#ifndef SCLIENT_COMMON_NET_SDPSESSIONDESCRIPTION_H
#define SCLIENT_COMMON_NET_SDPSESSIONDESCRIPTION_H

#include <string>

namespace sclient {
namespace common {
namespace net {

/**
 * RTP 视频会话描述
 *
 * 从 SDP (Session Description Protocol) 文件中解析出的视频流参数。
 * SDP 是描述多媒体会话的标准格式，常用于流媒体和 WebRTC。
 */
struct RtpVideoSessionDescription {
    std::string connection_address;  /**< 服务器地址（c= 行） */
    int video_port = 0;              /**< 视频端口号（m= 行） */
    int payload_type = -1;           /**< RTP 载荷类型（m= 行） */
    int clock_rate = 0;              /**< 时钟频率（a=rtpmap: 行） */
};

/**
 * 从 SDP 内容字符串解析视频会话描述
 *
 * @param contents SDP 文件内容
 * @param description 输出：解析结果
 * @param error_message 输出：错误信息（解析失败时）
 * @return 解析成功返回 true
 */
bool ParseRtpVideoSessionDescription(
        const std::string &contents,
        RtpVideoSessionDescription *description,
        std::string *error_message);

/**
 * 从 SDP 文件加载视频会话描述
 *
 * @param file_path SDP 文件路径
 * @param description 输出：解析结果
 * @param error_message 输出：错误信息（加载或解析失败时）
 * @return 加载成功返回 true
 */
bool LoadRtpVideoSessionDescription(
        const std::string &file_path,
        RtpVideoSessionDescription *description,
        std::string *error_message);

}  // namespace net
}  // namespace common
}  // namespace sclient

#endif  // SCLIENT_COMMON_NET_SDPSESSIONDESCRIPTION_H
