#ifndef SSERVER_COMMON_NET_STREAMPROTOCOL_H
#define SSERVER_COMMON_NET_STREAMPROTOCOL_H

#include <cstdint>
#include <cstring>

namespace sserver {
namespace common {
namespace net {

// TCP/UDP 私有应用层协议的消息类型。
//
// 该值写入 MessageHeader::message_type，接收端据此决定如何解释后续 payload。
// 枚举指定 uint16_t 底层类型，使其宽度与网络头字段保持一致。
enum class MessageType : std::uint16_t {
    // UDP 客户端注册/保活消息。由于 UDP 无连接，服务端需要通过此消息获知
    // 客户端地址并维护活跃状态；payload 还可携带 UdpReceiverReport。
    kKeepAlive = 0,
    // 编码后的音视频数据。TCP 下通常是元数据加完整帧；UDP 下通常是一个
    // UdpFrameFragmentHeader 加当前分片字节。
    kAvStream = 1,
    // UDP 丢失分片重传请求。payload 为 UdpNackHeader，后接若干 UdpNackItem。
    kUdpNack = 2,
};

// UDP 分片在一帧数据中的角色。
enum class UdpFragmentRole : std::uint16_t {
    // 普通媒体数据分片，内容可按 fragment_offset 放回完整帧缓冲区。
    kData = 0,
    // XOR FEC 奇偶校验分片。客户端丢失满足恢复条件的数据分片时，可以利用
    // parity 与其他已收到分片异或恢复，减少等待 NACK 重传带来的时延。
    kXorParity = 1,
};

// 自定义协议魔数，用来快速过滤不属于本协议的数据或检测流边界错位。
// ASCII 内容为 "CCTC"，每个 MessageHeader 都必须以这 4 字节开头。
// 魔数只能提供格式识别，不能代替加密、认证或完整性校验。
static const char kMessageMagic[4] = {'C', 'C', 'T', 'C'};

// 以下结构体当前会作为连续字节直接写入网络，所以用 pack(1) 禁止编译器
// 在字段之间插入对齐填充。push/pop 只在本段声明内改变对齐规则，避免影响
// 包含本头文件之后定义的其他结构体。
//
// 重要限制：
//   1. 当前协议直接传输主机字节序整数和本机 double 表示；
//   2. pack 只解决填充问题，不解决大小端、浮点格式和版本兼容；
//   3. 因此当前格式主要适合同架构服务端/客户端联调。
//
// 若要形成可长期维护的跨平台协议，应逐字段使用固定网络字节序序列化，
// 并加入协议版本、长度上限检查及必要的兼容策略。
#pragma pack(push, 1)
// 每条私有协议消息的通用头，pack(1) 下固定为 12 字节：
//
//   offset  size  field
//      0      4   head_id
//      4      2   message_type
//      6      2   sub_type
//      8      4   payload_length
//
// TCP 是连续字节流，接收端通常先精确读取 12 字节头，再按 payload_length
// 继续读取消息体；UDP datagram 则通常同时包含头和完整的该消息 payload。
struct MessageHeader {
    // 固定为 "CCTC"。
    char head_id[4];
    // MessageType 的底层整数值。结构体中不直接使用 enum class，是为了保持
    // 明确、简单的线格式；读写时应进行显式转换。
    std::uint16_t message_type;
    // 子类型或扩展用途。其语义由 message_type 决定，未使用时应写 0。
    std::uint16_t sub_type;
    // 紧随 MessageHeader 的 payload 字节数，不包含头部自身。
    // 接收端必须设置合理上限后再分配内存，不能无条件信任网络输入。
    std::uint32_t payload_length;
};

// TCP 等完整帧传输携带的诊断元数据，用于把端到端延迟拆成不同阶段。
// 时间戳单位都是纳秒，0 表示对应阶段没有提供有效时间。
struct FrameDiagnosticMetadata {
    // 逻辑媒体帧序号，用于关联服务端日志、传输消息与客户端渲染记录。
    std::uint64_t sequence;
    // 原始帧进入采集链路的单调时钟时间戳。
    std::uint64_t capture_timestamp_ns;
    // 编码器开始处理该帧的单调时钟时间戳。
    std::uint64_t encode_start_timestamp_ns;
    // 编码器完成该帧的单调时钟时间戳。
    std::uint64_t encode_end_timestamp_ns;
    // 发送端接近实际 socket 发送时记录的单调时钟时间戳。
    std::uint64_t transport_send_timestamp_ns;
};

// UDP 帧分片头。
//
// H.264 编码帧往往大于单个 UDP datagram 的安全载荷，因此服务端按 MTU
// 将一帧拆成多个分片。每个 datagram 都携带 MessageHeader、本结构和一段
// 媒体/FEC 字节；客户端以 frame_sequence 聚合，再按 offset/index 重组。
struct UdpFrameFragmentHeader {
    // 所属帧序号。
    std::uint64_t frame_sequence;
    // 每个分片重复携带帧级诊断时间戳。即使分片乱序到达，客户端也能在
    // 收到任一有效分片后建立该帧的诊断上下文。
    std::uint64_t capture_timestamp_ns;
    std::uint64_t encode_start_timestamp_ns;
    std::uint64_t encode_end_timestamp_ns;
    std::uint64_t transport_send_timestamp_ns;
    // 编码帧原始 payload 的总字节数，用于预分配重组缓冲区并校验边界。
    std::uint32_t frame_payload_size;
    // 当前数据分片在完整 payload 中的起始字节偏移。
    // FEC parity 分片不能简单按该偏移直接复制为媒体数据。
    std::uint32_t fragment_offset;
    // 当前分片下标，以及该帧普通数据分片的总数。
    // 接收端可用 fragment_index 标记到达状态、检测重复并生成 NACK。
    std::uint16_t fragment_index;
    std::uint16_t fragment_count;
    // UdpFragmentRole 的底层整数值，区分媒体数据与 XOR parity。
    std::uint16_t fragment_role;
    // 预留字段，发送端应置 0，接收端不应依赖其当前值。
    std::uint16_t reserved;
};

// UDP 客户端周期性上报的接收质量快照。
//
// 服务端可据此观察丢包、重复、乱序、重组超时和抖动，并为后续自适应码率、
// FEC 强度或发送节奏调整提供依据。当前字段多为客户端启动后的累计计数，
// 计算时间区间增量时需要保存并比较前后两次报告。
struct UdpReceiverReport {
    // 客户端生成本报告时的纳秒时间戳。
    std::uint64_t report_timestamp_ns;
    // 收到的 UDP datagram 总数，包括之后可能判定无效的数据报。
    std::uint64_t datagrams_received;
    // 因魔数、长度、字段范围或协议格式错误而拒绝的数据报数量。
    std::uint64_t invalid_datagrams;
    // 成功解析并接受的媒体/FEC 分片数量。
    std::uint64_t fragments_received;
    // 已经收到过相同帧序号与分片下标的重复分片数量。
    std::uint64_t duplicate_fragments;
    // 等待超过重组期限后仍未到达的分片估计数量。
    std::uint64_t timed_out_fragments;
    // 因分片不完整且超时而放弃重组的帧数量。
    std::uint64_t timed_out_frames;
    // 收齐数据、FEC 恢复或重传补齐后成功重组的完整帧数量。
    std::uint64_t completed_frames;
    // 完成顺序与帧序号预期不一致的帧数量，用于反映网络乱序。
    std::uint64_t reordered_frames;
    // 当前抖动统计包含的有效样本数量。
    std::uint64_t jitter_samples;
    // 最近一次、平均及最大网络抖动，单位为毫秒。
    double jitter_last_ms;
    double jitter_avg_ms;
    double jitter_max_ms;
};

// UDP NACK（Negative Acknowledgement）请求头。
//
// 内存/线格式为：
//   [UdpNackHeader][UdpNackItem 0]...[UdpNackItem request_count-1]
//
// 接收端发现分片缺失后发送该消息，服务端在重传缓存中查找对应分片。
struct UdpNackHeader {
    // 客户端生成重传请求的纳秒时间戳，可用于统计 NACK 处理延迟。
    std::uint64_t request_timestamp_ns;
    // 紧随头部的 UdpNackItem 数量，而不是字节数。
    std::uint16_t request_count;
    // 保留用于后续协议扩展，发送端应置 0。
    std::uint16_t reserved;
};

// 一条具体的缺失分片描述，由帧序号和分片下标共同定位。
struct UdpNackItem {
    std::uint64_t frame_sequence;
    std::uint16_t fragment_index;
    std::uint16_t reserved;
};
#pragma pack(pop)

inline void FillMessageMagic(char (&buffer)[4]) {
    // 数组引用参数要求调用者传入恰好 4 字节的 char 数组，避免退化成指针后
    // 无法获知容量。memcpy 不会追加字符串结尾 '\\0'，因为魔数是固定二进制
    // 字段，不是 C 风格字符串。
    std::memcpy(buffer, kMessageMagic, sizeof(kMessageMagic));
}

inline bool HasValidMessageMagic(const MessageHeader &header) {
    // 精确比较 4 个二进制字节，校验消息是否以 "CCTC" 开头。
    // 这里不能使用 strcmp，因为 head_id 没有空间保存结尾 '\\0'。
    return std::memcmp(header.head_id, kMessageMagic, sizeof(kMessageMagic)) == 0;
}

}  // namespace net
}  // namespace common
}  // namespace sserver

#endif  // SSERVER_COMMON_NET_STREAMPROTOCOL_H
