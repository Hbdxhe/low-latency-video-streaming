#ifndef SSERVER_MODULES_TRANSPORT_RTP_RTPPACKETIZER_H
#define SSERVER_MODULES_TRANSPORT_RTP_RTPPACKETIZER_H

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include "common/net/H264AnnexB.h"
#include "common/net/RtpProtocol.h"
#include "common/model/EncodedFrame.h"

namespace sserver {
namespace modules {
namespace transport {
namespace rtp {

// RTP 打包器：将编码后的 H264 帧按 RTP 协议拆分为多个 packet
// 支持单 NAL 直接打包和 FU-A 分片打包两种模式
class RtpPacketizer {
public:
    RtpPacketizer(
            std::uint8_t payload_type,       // RTP payload type，比如 96。必须和 SDP/客户端约定一致。
            std::uint32_t clock_rate,        // RTP 时钟频率。H.264 通常是 90000。
            std::size_t max_payload_size,    // 单个 RTP payload 最大大小，不包含 RTP header。
            std::uint32_t ssrc,              // RTP 同步源标识。同一路流通常固定一个 SSRC。
            bool enable_latency_extension);  // 是否启用项目自定义 RTP 延迟扩展。

    // 把一帧 EncodedFrame 打包成多个 RTP packet
    bool Packetize(
            common::model::EncodedFramePtr frame,
            std::vector<std::vector<std::uint8_t> > *packets,
            std::string *error_message);

private:
    // 根据 EncodedFrame 构造 RTP timestamp。
    //
    // 注意：
    // RTP timestamp 不是 Unix 时间，也不是纳秒时间。
    // 对 H.264 来说，它通常是 90kHz 媒体时钟。
    std::uint32_t BuildTimestamp(const common::model::EncodedFrame &frame) const;

    // 单个 NALU 可以完整放入一个 RTP 包时，走这个函数。
    //
    // 例如：一个 SPS 很小，只有几十字节，就直接：[RTP Header][完整 NALU]
    bool PacketizeSingleNalu(
            const std::uint8_t *nalu_data,   // NALU 起始地址，包含 NALU header。
            std::size_t nalu_size,           // NALU 总大小。
            bool marker,                     // 是否是当前帧最后一个 RTP 包。
            std::uint32_t timestamp,         // 当前帧的 RTP timestamp。
            const common::net::RtpHeaderExtension *header_extension, // 可选 RTP 扩展。
            std::vector<std::vector<std::uint8_t> > *packets);

    // 单个 NALU 太大，不能放进一个 RTP 包时，走 FU-A 分片。
    //
    // 例如：
    //   一个 IDR NALU 有 5000 字节，
    //   单个 RTP payload 只能放 1200 字节，就要拆成多个 FU-A RTP 包。
    bool PacketizeFragmentedNalu(
            const std::uint8_t *nalu_data,
            std::size_t nalu_size,
            bool marker,
            std::uint32_t timestamp,
            const common::net::RtpHeaderExtension *header_extension,
            std::vector<std::vector<std::uint8_t> > *packets,
            std::string *error_message);

private:
    // 常见动态类型从 96 开始，但 96 是否表示 H.264 要靠 SDP/配置约定
    std::uint8_t payload_type_;

    // RTP 时钟频率。
    // H.264 通常是 90000。
    std::uint32_t clock_rate_;

    // 单个 RTP payload 最大大小。
    // 注意：这里是不包含 RTP header 的媒体 payload 大小。
    std::size_t max_payload_size_;

    // RTP SSRC，同一路 RTP 流的标识
    std::uint32_t ssrc_;

    // 是否给 RTP 包加项目自定义延迟扩展
    bool enable_latency_extension_;

    // RTP sequence number。
    // 每生成一个 RTP 包就递增一次。
    //
    // 注意：不是每帧递增一次，而是每个 RTP packet 递增一次。
    std::uint16_t sequence_number_;

    // 临时保存从 Annex-B H.264 payload 中切出来的 NALU 视图。
    //
    // 例如一帧 payload 里可能包含：
    //   SPS, PPS, IDR
    //
    // SplitAnnexBNalus 会把它们拆成多个 H264NaluView。
    std::vector<common::net::H264NaluView> nalus_;
};

}  // namespace rtp
}  // namespace transport
}  // namespace modules
}  // namespace sserver

#endif  // SSERVER_MODULES_TRANSPORT_RTP_RTPPACKETIZER_H
