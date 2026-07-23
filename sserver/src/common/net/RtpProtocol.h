#ifndef SSERVER_COMMON_NET_RTPPROTOCOL_H
#define SSERVER_COMMON_NET_RTPPROTOCOL_H

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <vector>

namespace sserver {
namespace common {
namespace net {

// RTP（Real-time Transport Protocol）固定头的最小长度为 12 字节。
// 若携带 CSRC、header extension 或 padding，实际头部/包长度会更大。
static const std::size_t kRtpHeaderSize = 12;

// RFC 3550 扩展头固定部分：profile id 2 字节 + 长度 2 字节。
// 这里不包含扩展 payload 本身。
static const std::size_t kRtpExtensionHeaderSize = 4;

// RTP 当前标准版本，写入固定头第 0 字节最高 2 bit。
static const std::uint8_t kRtpVersion = 2;

// RFC 6184 定义的 H.264 FU-A（Fragmentation Unit type A）NAL unit type。
// 当一个 NALU 超过单个 RTP 包可承载大小时，用 FU-A 拆成多个 RTP 包。
static const std::uint8_t kH264FuAType = 28;

// 项目自定义延迟扩展的 profile id，0x5353 的 ASCII 可理解为 "SS"。
// 它不是 IANA 标准扩展格式，仅用于本项目服务端与客户端约定。
static const std::uint16_t kRtpLatencyExtensionProfileId = 0x5353;

// 延迟扩展依次包含两个 uint64_t 纳秒时间戳，共 16 字节。
static const std::size_t kRtpLatencyExtensionPayloadSize = 16;

// RTP 扩展长度字段以 32 bit word 为单位，16 字节等于 4 个 word。
static const std::size_t kRtpLatencyExtensionWordSize = kRtpLatencyExtensionPayloadSize / 4;

// 开启本项目延迟扩展时，单个 RTP 包除媒体 payload 外的固定开销：
//   12 字节 RTP 固定头 + 4 字节扩展头 + 16 字节扩展内容 = 32 字节。
// packetizer 需要从 MTU 预算中扣除该开销，避免产生 IP 分片。
static const std::size_t kRtpPacketOverheadWithLatencyExtension =
        kRtpHeaderSize + kRtpExtensionHeaderSize + kRtpLatencyExtensionPayloadSize;

// RTP 固定头中本项目需要读写的字段。
struct RtpHeaderFields {
    // Marker 位（M）。
    // H.264 视频中通常仅在“当前访问单元/视频帧的最后一个 RTP 包”上置 1，
    // 接收端可据此判断一帧的包边界，但仍需结合 sequence number 处理丢包。
    bool marker = false;

    // 7 bit Payload Type（PT）。动态负载类型常从 96 开始分配，
    // 具体数值必须与 SDP 或双方配置一致，96 并不天然等于 H.264。
    std::uint8_t payload_type = 96;

    // RTP 包序号。每发送一个 RTP 包递增一次，而不是每视频帧递增一次。
    // 接收端用它检测丢包、重复和乱序；16 bit 从 65535 到 0 自然回绕。
    std::uint16_t sequence_number = 0;

    // 媒体时间戳，不是 Unix 时间或纳秒时间。
    // H.264 通常使用 90 kHz 时钟；同一视频帧拆出的所有 RTP 包使用相同值。
    std::uint32_t timestamp = 0;

    // Synchronization Source 标识（SSRC）。
    // 同一 RTP 流通常保持相同 SSRC，接收端据此区分不同同步源。
    std::uint32_t ssrc = 0;
};

// RTP header extension 的通用内存表示。
// profile_id 决定 payload 的解释方式，本结构本身不理解 payload 内容。
struct RtpHeaderExtension {
    // 扩展格式/profile 标识。
    std::uint16_t profile_id = 0;

    // 扩展 payload。RTP 头中的扩展长度以 32 bit word 编码，因此这里的
    // 字节数必须是 4 的倍数；WriteRtpHeader() 会对此进行校验。
    std::vector<std::uint8_t> payload;
};

// 项目自定义 RTP 延迟诊断扩展。
struct RtpLatencyExtension {
    // 原始视频帧进入采集链路的纳秒时间戳。
    std::uint64_t capture_timestamp_ns = 0;

    // 当前 RTP 包实际调用发送操作前写入的纳秒时间戳。
    std::uint64_t transport_send_timestamp_ns = 0;
};

// RTP 网络字段统一使用网络字节序，即大端序：高有效字节排在前面。
// 以下函数通过移位显式读写，避免直接 reinterpret_cast 导致主机字节序、
// 未对齐访问和结构体填充问题。调用者必须保证 data 指向足够大的有效内存。
inline std::uint16_t ReadBe16(const std::uint8_t *data) {
    return static_cast<std::uint16_t>((static_cast<std::uint16_t>(data[0]) << 8) |
                                      static_cast<std::uint16_t>(data[1]));
}

inline std::uint32_t ReadBe32(const std::uint8_t *data) {
    return (static_cast<std::uint32_t>(data[0]) << 24) |
            (static_cast<std::uint32_t>(data[1]) << 16) |
            (static_cast<std::uint32_t>(data[2]) << 8) |
            static_cast<std::uint32_t>(data[3]);
}

inline std::uint64_t ReadBe64(const std::uint8_t *data) {
    return (static_cast<std::uint64_t>(data[0]) << 56) |
           (static_cast<std::uint64_t>(data[1]) << 48) |
           (static_cast<std::uint64_t>(data[2]) << 40) |
           (static_cast<std::uint64_t>(data[3]) << 32) |
           (static_cast<std::uint64_t>(data[4]) << 24) |
           (static_cast<std::uint64_t>(data[5]) << 16) |
           (static_cast<std::uint64_t>(data[6]) << 8) |
           static_cast<std::uint64_t>(data[7]);
}

inline void WriteBe16(std::uint16_t value, std::uint8_t *data) {
    data[0] = static_cast<std::uint8_t>((value >> 8) & 0xFF);
    data[1] = static_cast<std::uint8_t>(value & 0xFF);
}

inline void WriteBe32(std::uint32_t value, std::uint8_t *data) {
    data[0] = static_cast<std::uint8_t>((value >> 24) & 0xFF);
    data[1] = static_cast<std::uint8_t>((value >> 16) & 0xFF);
    data[2] = static_cast<std::uint8_t>((value >> 8) & 0xFF);
    data[3] = static_cast<std::uint8_t>(value & 0xFF);
}

inline void WriteBe64(std::uint64_t value, std::uint8_t *data) {
    data[0] = static_cast<std::uint8_t>((value >> 56) & 0xFF);
    data[1] = static_cast<std::uint8_t>((value >> 48) & 0xFF);
    data[2] = static_cast<std::uint8_t>((value >> 40) & 0xFF);
    data[3] = static_cast<std::uint8_t>((value >> 32) & 0xFF);
    data[4] = static_cast<std::uint8_t>((value >> 24) & 0xFF);
    data[5] = static_cast<std::uint8_t>((value >> 16) & 0xFF);
    data[6] = static_cast<std::uint8_t>((value >> 8) & 0xFF);
    data[7] = static_cast<std::uint8_t>(value & 0xFF);
}

inline std::array<std::uint8_t, kRtpLatencyExtensionPayloadSize> SerializeRtpLatencyExtension(
        const RtpLatencyExtension &extension) {
    // 序列化后的固定布局：
    //   offset 0..7  : capture_timestamp_ns
    //   offset 8..15 : transport_send_timestamp_ns
    // 两个字段都按大端序写入，不能直接 memcpy 原始结构体。
    std::array<std::uint8_t, kRtpLatencyExtensionPayloadSize> payload {};
    WriteBe64(extension.capture_timestamp_ns, payload.data());
    WriteBe64(extension.transport_send_timestamp_ns, payload.data() + 8);
    return payload;
}

inline RtpHeaderExtension BuildRtpLatencyHeaderExtension(const RtpLatencyExtension &extension) {
    // 将有明确字段语义的延迟结构包装成 WriteRtpHeader() 可接受的通用扩展。
    RtpHeaderExtension header_extension;
    header_extension.profile_id = kRtpLatencyExtensionProfileId;
    const std::array<std::uint8_t, kRtpLatencyExtensionPayloadSize> payload =
            SerializeRtpLatencyExtension(extension);
    header_extension.payload.assign(payload.begin(), payload.end());
    return header_extension;
}

inline bool ParseRtpLatencyExtension(
        const RtpHeaderExtension &header_extension,
        RtpLatencyExtension *extension) {
    // 只有 profile id 和 payload 长度都完全匹配时才能按本项目格式解析。
    // 返回 false 表示“不是有效的本项目延迟扩展”，不会输出部分结果。
    if (extension == nullptr ||
        header_extension.profile_id != kRtpLatencyExtensionProfileId ||
        header_extension.payload.size() != kRtpLatencyExtensionPayloadSize) {
        return false;
    }

    extension->capture_timestamp_ns = ReadBe64(header_extension.payload.data());
    extension->transport_send_timestamp_ns = ReadBe64(header_extension.payload.data() + 8);
    return true;
}

inline bool WriteRtpHeader(
        const RtpHeaderFields &fields,
        const RtpHeaderExtension *extension,
        std::vector<std::uint8_t> *packet) {
    // 构造 RTP 固定头和可选扩展，不写入 H.264 媒体 payload。
    // 成功后 packet 的内容布局为：
    //
    //   [12 字节固定头][可选 4 字节扩展头][可选扩展 payload]
    //
    // packetizer 随后通过 insert 等方式追加单 NALU 或 FU-A payload。
    if (packet == nullptr) {
        return false;
    }

    // 仅传入非空扩展 payload 时才设置 RTP 的 X 位。
    const bool has_extension = extension != nullptr && !extension->payload.empty();
    // RTP 扩展长度以 32 bit word 表示，因此 payload 必须按 4 字节对齐。
    if (has_extension && (extension->payload.size() % 4) != 0) {
        return false;
    }

    const std::size_t extension_size =
            has_extension ? (kRtpExtensionHeaderSize + extension->payload.size()) : 0;
    // assign 同时调整 vector 大小并将全部字节清零，未显式设置的 P、CC 位为 0。
    packet->assign(kRtpHeaderSize + extension_size, 0);
    // 固定头第 0 字节：V(2 bit)、P(1 bit)、X(1 bit)、CC(4 bit)。
    (*packet)[0] = static_cast<std::uint8_t>((kRtpVersion << 6) | (has_extension ? 0x10 : 0x00));
    // 固定头第 1 字节：M(1 bit) + PT(7 bit)。
    (*packet)[1] = static_cast<std::uint8_t>((fields.marker ? 0x80 : 0x00) | (fields.payload_type & 0x7F));
    WriteBe16(fields.sequence_number, packet->data() + 2);
    WriteBe32(fields.timestamp, packet->data() + 4);
    WriteBe32(fields.ssrc, packet->data() + 8);

    if (has_extension) {
        // RTP 扩展头：
        //   前 2 字节是 profile id；
        //   后 2 字节记录扩展 payload 包含多少个 32 bit word。
        WriteBe16(extension->profile_id, packet->data() + kRtpHeaderSize);
        WriteBe16(
                static_cast<std::uint16_t>(extension->payload.size() / 4),
                packet->data() + kRtpHeaderSize + 2);
        std::copy(extension->payload.begin(), extension->payload.end(), packet->data() + kRtpHeaderSize + 4);
    }

    return true;
}

inline void WriteRtpHeader(const RtpHeaderFields &fields, std::vector<std::uint8_t> *packet) {
    // 不携带扩展时的便捷重载。忽略 bool 返回值是安全的，因为这里传入的
    // extension 为 nullptr，不存在扩展长度不对齐问题；packet 仍不可为 nullptr。
    WriteRtpHeader(fields, nullptr, packet);
}

inline bool ParseRtpHeader(
        const std::uint8_t *data,
        std::size_t size,
        RtpHeaderFields *fields,
        std::size_t *header_size,
        RtpHeaderExtension *extension = nullptr) {
    // 解析并校验 RTP 固定头、CSRC 列表和可选扩展。
    //
    // 成功时：
    //   fields      <- 固定头字段；
    //   header_size <- 媒体 payload 在 data 中的起始偏移；
    //   extension   <- 可选扩展的 profile 和字节内容。
    //
    // 该函数不会复制 RTP 媒体 payload。调用者可用：
    //   data + *header_size
    // 找到 H.264 RTP payload。
    if (data == nullptr || fields == nullptr || size < kRtpHeaderSize) {
        return false;
    }

    const std::uint8_t version = static_cast<std::uint8_t>(data[0] >> 6);
    const bool has_padding = (data[0] & 0x20) != 0;
    const bool has_extension = (data[0] & 0x10) != 0;
    const std::uint8_t csrc_count = static_cast<std::uint8_t>(data[0] & 0x0F);
    // 每个 CSRC 占 4 字节。
    const std::size_t base_header_size = kRtpHeaderSize + static_cast<std::size_t>(csrc_count) * 4;
    // 当前项目没有实现包尾 padding 的长度解析，因此明确拒绝 P=1 的包。
    if (version != kRtpVersion || has_padding || size < base_header_size) {
        return false;
    }

    std::size_t computed_header_size = base_header_size;
    if (has_extension) {
        if (size < base_header_size + kRtpExtensionHeaderSize) {
            return false;
        }

        // 扩展长度字段单位是 32 bit word，乘 4 后得到字节数。
        const std::uint16_t extension_words = ReadBe16(data + base_header_size + 2);
        const std::size_t extension_payload_size = static_cast<std::size_t>(extension_words) * 4;
        computed_header_size += kRtpExtensionHeaderSize + extension_payload_size;
        if (size < computed_header_size) {
            return false;
        }

        if (extension != nullptr) {
            extension->profile_id = ReadBe16(data + base_header_size);
            extension->payload.assign(
                    data + base_header_size + kRtpExtensionHeaderSize,
                    data + base_header_size + kRtpExtensionHeaderSize + extension_payload_size);
        }
    } else if (extension != nullptr) {
        extension->profile_id = 0;
        extension->payload.clear();
    }

    // 层层边界检查完成后，再读取固定头字段，避免越界访问。
    fields->marker = (data[1] & 0x80) != 0;
    fields->payload_type = static_cast<std::uint8_t>(data[1] & 0x7F);
    fields->sequence_number = ReadBe16(data + 2);
    fields->timestamp = ReadBe32(data + 4);
    fields->ssrc = ReadBe32(data + 8);
    if (header_size != nullptr) {
        *header_size = computed_header_size;
    }
    return true;
}

inline bool UpdateRtpLatencyExtensionTransportSendTimestamp(
        std::uint8_t *data,
        std::size_t size,
        std::uint64_t transport_send_timestamp_ns) {
    // RTP 包可能在发送队列中提前完成 packetize，但只有真正发送前才能得到
    // 更准确的 transport_send_timestamp_ns。该函数直接修改 packet vector
    // 底层字节中的第二个 uint64_t，避免重新构造头部和复制媒体 payload。
    //
    // 修改前会验证 RTP 版本、X/P 位、CSRC 长度、profile id 和扩展长度，
    // 防止把时间戳误写进普通 RTP 扩展或媒体数据。
    if (data == nullptr || size < kRtpHeaderSize + kRtpExtensionHeaderSize + kRtpLatencyExtensionPayloadSize) {
        return false;
    }

    const std::uint8_t version = static_cast<std::uint8_t>(data[0] >> 6);
    const bool has_padding = (data[0] & 0x20) != 0;
    const bool has_extension = (data[0] & 0x10) != 0;
    const std::uint8_t csrc_count = static_cast<std::uint8_t>(data[0] & 0x0F);
    const std::size_t base_header_size = kRtpHeaderSize + static_cast<std::size_t>(csrc_count) * 4;
    if (version != kRtpVersion || has_padding || !has_extension || size < base_header_size + kRtpExtensionHeaderSize) {
        return false;
    }

    // 确认该扩展确实是本项目定义的 16 字节、4 word 延迟扩展。
    if (ReadBe16(data + base_header_size) != kRtpLatencyExtensionProfileId ||
        ReadBe16(data + base_header_size + 2) != kRtpLatencyExtensionWordSize ||
        size < base_header_size + kRtpExtensionHeaderSize + kRtpLatencyExtensionPayloadSize) {
        return false;
    }

    // 扩展 payload 前 8 字节保持 capture timestamp 不变，只覆盖后 8 字节。
    WriteBe64(transport_send_timestamp_ns, data + base_header_size + kRtpExtensionHeaderSize + 8);
    return true;
}

}  // namespace net
}  // namespace common
}  // namespace sserver

#endif  // SSERVER_COMMON_NET_RTPPROTOCOL_H
