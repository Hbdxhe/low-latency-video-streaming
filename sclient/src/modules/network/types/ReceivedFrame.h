#ifndef SCLIENT_MODULES_NETWORK_TYPES_RECEIVEDFRAME_H
#define SCLIENT_MODULES_NETWORK_TYPES_RECEIVEDFRAME_H

#include <cstdint>
#include <vector>

#include "common/protocol/Protocol.h"

namespace sclient {

/** 接收到的视频帧 */
struct ReceivedFrame {
    protocol::MessageHeader header;               /**< 消息头部 */
    protocol::FrameDiagnosticMetadata metadata;   /**< 发送端诊断元数据 */
    bool sender_metadata_available = false;       /**< 发送端元数据是否可用 */
    std::uint64_t receive_timestamp_ns = 0;       /**< 接收时间戳（纳秒） */
    std::vector<std::uint8_t> payload;            /**< 帧载荷数据（编码后的视频帧） */
};

}  // namespace sclient

#endif  // SCLIENT_MODULES_NETWORK_TYPES_RECEIVEDFRAME_H
