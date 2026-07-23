#ifndef SCLIENT_COMMON_NET_RTPPROTOCOL_H
#define SCLIENT_COMMON_NET_RTPPROTOCOL_H

#include <array>
#include <cstddef>
#include <cstdint>
#include <vector>

namespace sclient {
namespace common {
namespace net {

/**
 * RTP 协议常量定义
 *
 * RTP (Real-time Transport Protocol) 是实时音视频传输的标准协议。
 * 参考 RFC 3550。
 */

static const std::size_t kRtpHeaderSize = 12;               /**< RTP 固定头部大小（字节） */
static const std::size_t kRtpExtensionHeaderSize = 4;       /**< RTP 扩展头部大小（profile_id + length） */
static const std::uint8_t kRtpVersion = 2;                  /**< RTP 版本号（当前固定为2） */
static const std::uint8_t kH264FuAType = 28;                /**< H.264 FU-A 分片类型（RFC 6184） */
static const std::uint16_t kRtpLatencyExtensionProfileId = 0x5353;  /**< 自定义延迟扩展的 profile ID */
static const std::size_t kRtpLatencyExtensionPayloadSize = 16;      /**< 延迟扩展载荷大小（2个64位时间戳） */
static const std::size_t kRtpLatencyExtensionWordSize = kRtpLatencyExtensionPayloadSize / 4; /**< 载荷大小（以32位字为单位） */

/**
 * RTP 头部字段
 *
 * 包含 RTP 包头中解析出的关键信息
 */
struct RtpHeaderFields {
    bool marker = false;                /**< 标记位，通常用于标识帧边界 */
    std::uint8_t payload_type = 96;     /**< 载荷类型，96-127为动态类型 */
    std::uint16_t sequence_number = 0;  /**< 序列号，用于检测丢包和重排序 */
    std::uint32_t timestamp = 0;        /**< 时间戳，标识媒体采样时刻 */
    std::uint32_t ssrc = 0;             /**< 同步源标识符 */
};

/** RTP 头部扩展 */
struct RtpHeaderExtension {
    std::uint16_t profile_id = 0;       /**< 扩展 profile 标识 */
    std::vector<std::uint8_t> payload;  /**< 扩展载荷数据 */
};

/**
 * 自定义延迟扩展
 *
 * 用于在 RTP 包中携带采集和发送时间戳，实现端到端延迟测量
 */
struct RtpLatencyExtension {
    std::uint64_t capture_timestamp_ns = 0;           /**< 采集时间戳（纳秒） */
    std::uint64_t transport_send_timestamp_ns = 0;    /**< 发送时间戳（纳秒） */
};

/**
 * 大端字节序读写工具函数
 *
 * RTP 协议使用网络字节序（大端），而 x86/x64 架构使用小端字节序，
 * 因此需要进行字节序转换。
 */

/** 从大端字节序读取16位整数 */
inline std::uint16_t ReadBe16(const std::uint8_t *data) {
    return static_cast<std::uint16_t>((static_cast<std::uint16_t>(data[0]) << 8) |
                                      static_cast<std::uint16_t>(data[1]));
}

/** 从大端字节序读取32位整数 */
inline std::uint32_t ReadBe32(const std::uint8_t *data) {
    return (static_cast<std::uint32_t>(data[0]) << 24) |
           (static_cast<std::uint32_t>(data[1]) << 16) |
           (static_cast<std::uint32_t>(data[2]) << 8) |
           static_cast<std::uint32_t>(data[3]);
}

/** 从大端字节序读取64位整数 */
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

/** 将16位整数写入大端字节序 */
inline void WriteBe16(std::uint16_t value, std::uint8_t *data) {
    data[0] = static_cast<std::uint8_t>((value >> 8) & 0xFF);
    data[1] = static_cast<std::uint8_t>(value & 0xFF);
}

/** 将32位整数写入大端字节序 */
inline void WriteBe32(std::uint32_t value, std::uint8_t *data) {
    data[0] = static_cast<std::uint8_t>((value >> 24) & 0xFF);
    data[1] = static_cast<std::uint8_t>((value >> 16) & 0xFF);
    data[2] = static_cast<std::uint8_t>((value >> 8) & 0xFF);
    data[3] = static_cast<std::uint8_t>(value & 0xFF);
}

/** 将64位整数写入大端字节序 */
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

/** 序列化延迟扩展为字节数组 */
inline std::array<std::uint8_t, kRtpLatencyExtensionPayloadSize> SerializeRtpLatencyExtension(
        const RtpLatencyExtension &extension) {
    std::array<std::uint8_t, kRtpLatencyExtensionPayloadSize> payload {};
    WriteBe64(extension.capture_timestamp_ns, payload.data());
    WriteBe64(extension.transport_send_timestamp_ns, payload.data() + 8);
    return payload;
}

/** 构建带延迟扩展的 RTP 头部扩展 */
inline RtpHeaderExtension BuildRtpLatencyHeaderExtension(const RtpLatencyExtension &extension) {
    RtpHeaderExtension header_extension;
    header_extension.profile_id = kRtpLatencyExtensionProfileId;
    const std::array<std::uint8_t, kRtpLatencyExtensionPayloadSize> payload =
            SerializeRtpLatencyExtension(extension);
    header_extension.payload.assign(payload.begin(), payload.end());
    return header_extension;
}

/** 从 RTP 头部扩展解析延迟信息 */
inline bool ParseRtpLatencyExtension(
        const RtpHeaderExtension &header_extension,
        RtpLatencyExtension *extension) {
    if (extension == nullptr ||
        header_extension.profile_id != kRtpLatencyExtensionProfileId ||
        header_extension.payload.size() != kRtpLatencyExtensionPayloadSize) {
        return false;
    }

    extension->capture_timestamp_ns = ReadBe64(header_extension.payload.data());
    extension->transport_send_timestamp_ns = ReadBe64(header_extension.payload.data() + 8);
    return true;
}

/**
 * 写入 RTP 头部
 *
 * @param fields 头部字段
 * @param extension 可选的头部扩展，为 nullptr 表示不添加扩展
 * @param packet 输出缓冲区
 * @return 成功返回 true，参数无效返回 false
 *
 * 注意：扩展载荷长度必须是4字节的倍数（RTP协议要求）
 */
inline bool WriteRtpHeader(
        const RtpHeaderFields &fields,
        const RtpHeaderExtension *extension,
        std::vector<std::uint8_t> *packet) {
    if (packet == nullptr) {
        return false;
    }

    const bool has_extension = extension != nullptr && !extension->payload.empty();
    if (has_extension && (extension->payload.size() % 4) != 0) {
        return false;
    }

    const std::size_t extension_size =
            has_extension ? (kRtpExtensionHeaderSize + extension->payload.size()) : 0;
    packet->assign(kRtpHeaderSize + extension_size, 0);
    // 第1字节：版本号(2bit) + 填充标志(1bit) + 扩展标志(1bit) + CSRC计数(4bit)
    (*packet)[0] = static_cast<std::uint8_t>((kRtpVersion << 6) | (has_extension ? 0x10 : 0x00));
    // 第2字节：标记位(1bit) + 载荷类型(7bit)
    (*packet)[1] = static_cast<std::uint8_t>((fields.marker ? 0x80 : 0x00) | (fields.payload_type & 0x7F));
    WriteBe16(fields.sequence_number, packet->data() + 2);
    WriteBe32(fields.timestamp, packet->data() + 4);
    WriteBe32(fields.ssrc, packet->data() + 8);

    // 写入扩展头部
    if (has_extension) {
        WriteBe16(extension->profile_id, packet->data() + kRtpHeaderSize);
        WriteBe16(
                static_cast<std::uint16_t>(extension->payload.size() / 4),
                packet->data() + kRtpHeaderSize + 2);
        std::copy(extension->payload.begin(), extension->payload.end(), packet->data() + kRtpHeaderSize + 4);
    }

    return true;
}

/** 写入不带扩展的 RTP 头部 */
inline void WriteRtpHeader(const RtpHeaderFields &fields, std::vector<std::uint8_t> *packet) {
    WriteRtpHeader(fields, nullptr, packet);
}

/**
 * 解析 RTP 头部
 *
 * @param data 原始数据
 * @param size 数据大小
 * @param fields 输出：解析出的头部字段
 * @param header_size 输出：头部总大小（含CSRC和扩展）
 * @param extension 输出：头部扩展（可选，传 nullptr 则忽略扩展）
 * @return 解析成功返回 true
 *
 * 支持解析带 CSRC 和头部扩展的 RTP 包。
 * 不支持带填充（padding）的包，因为本项目不使用填充。
 */
inline bool ParseRtpHeader(
        const std::uint8_t *data,
        std::size_t size,
        RtpHeaderFields *fields,
        std::size_t *header_size,
        RtpHeaderExtension *extension = nullptr) {
    if (data == nullptr || fields == nullptr || size < kRtpHeaderSize) {
        return false;
    }

    // 解析第1字节：版本号、填充标志、扩展标志、CSRC计数
    const std::uint8_t version = static_cast<std::uint8_t>(data[0] >> 6);
    const bool has_padding = (data[0] & 0x20) != 0;
    const bool has_extension = (data[0] & 0x10) != 0;
    const std::uint8_t csrc_count = static_cast<std::uint8_t>(data[0] & 0x0F);
    const std::size_t base_header_size = kRtpHeaderSize + static_cast<std::size_t>(csrc_count) * 4;
    if (version != kRtpVersion || has_padding || size < base_header_size) {
        return false;
    }

    // 解析扩展头部（如果存在）
    std::size_t computed_header_size = base_header_size;
    if (has_extension) {
        if (size < base_header_size + kRtpExtensionHeaderSize) {
            return false;
        }

        // 扩展头部：profile_id(2字节) + 长度(2字节，以32位字为单位)
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

    // 解析固定头部字段
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

}  // namespace net
}  // namespace common
}  // namespace sclient

#endif  // SCLIENT_COMMON_NET_RTPPROTOCOL_H
