#ifndef SCLIENT_PROTOCOL_H
#define SCLIENT_PROTOCOL_H

#include <cstdint>
#include <cstring>

namespace sclient {
namespace protocol {

/**
 * 自定义传输协议定义
 *
 * 本文件定义了客户端与服务器之间的二进制协议格式。
 * 所有结构体使用 #pragma pack(1) 确保字节对齐与网络传输一致。
 */

/** 消息类型 */
enum class MessageType : std::uint16_t {
    kKeepAlive = 0,  /**< 心跳保活消息 */
    kAvStream = 1,   /**< 音视频流数据 */
    kUdpNack = 2,    /**< UDP 丢包重传请求 */
};

/** UDP 分片角色 */
enum class UdpFragmentRole : std::uint16_t {
    kData = 0,       /**< 数据分片 */
    kXorParity = 1,  /**< XOR 校验分片（用于前向纠错） */
};

static const char kMessageMagic[4] = {'C', 'C', 'T', 'C'};  /**< 消息魔数，用于校验 */

#pragma pack(push, 1)

/**
 * 消息头部（12字节）
 *
 * 所有消息都以此头部开头，用于识别消息类型和载荷长度
 */
struct MessageHeader {
    char head_id[4];              /**< 魔数标识，固定为 "CCTC" */
    std::uint16_t message_type;   /**< 消息类型（MessageType） */
    std::uint16_t sub_type;       /**< 子类型，保留字段 */
    std::uint32_t payload_length; /**< 载荷长度（字节） */
};

/**
 * 帧诊断元数据
 *
 * 用于端到端延迟测量和性能分析
 */
struct FrameDiagnosticMetadata {
    std::uint64_t sequence;                    /**< 帧序列号 */
    std::uint64_t capture_timestamp_ns;        /**< 采集时间戳（纳秒） */
    std::uint64_t encode_start_timestamp_ns;   /**< 编码开始时间戳 */
    std::uint64_t encode_end_timestamp_ns;     /**< 编码结束时间戳 */
    std::uint64_t transport_send_timestamp_ns; /**< 发送时间戳 */
};

/**
 * UDP 帧分片头部
 *
 * 大帧会被分割成多个 UDP 分片传输，此头部描述分片所属的帧和分片信息。
 * XOR 校验分片用于前向纠错，可恢复单个丢失的数据分片。
 */
struct UdpFrameFragmentHeader {
    std::uint64_t frame_sequence;              /**< 帧序列号 */
    std::uint64_t capture_timestamp_ns;        /**< 采集时间戳 */
    std::uint64_t encode_start_timestamp_ns;   /**< 编码开始时间戳 */
    std::uint64_t encode_end_timestamp_ns;     /**< 编码结束时间戳 */
    std::uint64_t transport_send_timestamp_ns; /**< 发送时间戳 */
    std::uint32_t frame_payload_size;          /**< 整帧载荷大小 */
    std::uint32_t fragment_offset;             /**< 当前分片在帧中的偏移 */
    std::uint16_t fragment_index;              /**< 分片索引（从0开始） */
    std::uint16_t fragment_count;              /**< 总分片数 */
    std::uint16_t fragment_role;               /**< 分片角色（UdpFragmentRole） */
    std::uint16_t reserved;                    /**< 保留字段 */
};

/**
 * UDP 接收报告
 *
 * 客户端定期发送给服务器的接收质量统计。
 * 用于服务器端的拥塞控制和质量监控。
 *
 * 注意：double 字段通过 memcpy 原始传输，要求收发双方使用相同的浮点表示（IEEE 754）
 */
struct UdpReceiverReport {
    std::uint64_t report_timestamp_ns;    /**< 报告生成时间戳 */
    std::uint64_t datagrams_received;     /**< 收到的 UDP 数据报总数 */
    std::uint64_t invalid_datagrams;      /**< 无效数据报数 */
    std::uint64_t fragments_received;     /**< 收到的分片总数 */
    std::uint64_t duplicate_fragments;    /**< 重复分片数 */
    std::uint64_t timed_out_fragments;    /**< 超时未到的分片数 */
    std::uint64_t timed_out_frames;       /**< 超时未完整的帧数 */
    std::uint64_t completed_frames;       /**< 完整接收的帧数 */
    std::uint64_t reordered_frames;       /**< 经过重排序的帧数 */
    std::uint64_t jitter_samples;         /**< 抖动采样数 */
    double jitter_last_ms;                /**< 最近一次抖动值（毫秒） */
    double jitter_avg_ms;                 /**< 平均抖动 */
    double jitter_max_ms;                 /**< 最大抖动 */
};

/**
 * UDP NACK（Negative Acknowledgment）请求头部
 *
 * 客户端检测到丢包后，向服务器发送 NACK 请求重传丢失的分片
 */
struct UdpNackHeader {
    std::uint64_t request_timestamp_ns;  /**< 请求时间戳 */
    std::uint16_t request_count;         /**< 请求重传的分片数量 */
    std::uint16_t reserved;              /**< 保留字段 */
};

/** NACK 请求项：指定需要重传的帧和分片 */
struct UdpNackItem {
    std::uint64_t frame_sequence;   /**< 帧序列号 */
    std::uint16_t fragment_index;   /**< 分片索引 */
    std::uint16_t reserved;         /**< 保留字段 */
};
#pragma pack(pop)

/** 填充消息魔数 */
inline void FillMessageMagic(char (&buffer)[4]) {
    std::memcpy(buffer, kMessageMagic, sizeof(kMessageMagic));
}

/** 验证消息魔数是否正确 */
inline bool HasValidMessageMagic(const MessageHeader &header) {
    return std::memcmp(header.head_id, kMessageMagic, sizeof(kMessageMagic)) == 0;
}

}  // namespace protocol
}  // namespace sclient

#endif  // SCLIENT_PROTOCOL_H
