#include "modules/transport/rtp/RtpPacketizer.h"

#include <algorithm>
#include <chrono>
#include <cstring>
#include <limits>
#include <random>

#include "common/net/H264AnnexB.h"
#include "common/net/RtpProtocol.h"

namespace sserver {
namespace modules {
namespace transport {
namespace rtp {

namespace {

// 生成 RTP 初始 sequence number。
//
// RTP 不要求 sequence number 必须从 0 开始。
// 通常会随机一个初始值，这样更符合 RTP 习惯，也避免不同会话太容易混淆。
std::uint16_t BuildInitialSequenceNumber(std::uint32_t ssrc) {
    // 获取当前单调时钟的计数值
    const std::uint64_t now_ns = static_cast<std::uint64_t>(
            std::chrono::steady_clock::now().time_since_epoch().count());

    // 用时间低 32 位、时间高 32 位、ssrc 混合成随机种子
    // 这样不同启动时刻、不同 SSRC，初始 sequence number 更可能不同
    const std::uint32_t seed = static_cast<std::uint32_t>(now_ns & 0xFFFFFFFFu) ^
                               static_cast<std::uint32_t>(now_ns >> 32) ^
                               ssrc;

    // 伪随机数生成器
    std::mt19937 generator(seed);

    // 随机范围：0 到 uint16_t 最大值，也就是 65535
    std::uniform_int_distribution<std::uint32_t> distribution(
            0,
            static_cast<std::uint32_t>(std::numeric_limits<std::uint16_t>::max()));

    // 返回 uint16_t 类型的随机 sequence number
    return static_cast<std::uint16_t>(distribution(generator));
}

// 直接把 RTP header 写到已有 buffer 里。
//
// 这个函数和 RtpProtocol.h 里的 WriteRtpHeader 类似，
// 但这里是为了 FU-A 分片场景做优化：
// 先 resize 出完整 packet，然后直接往指定位置写 header 和 payload。
void WriteRtpHeaderToBuffer(
        std::uint8_t *buffer,
        const common::net::RtpHeaderFields &fields,
        const common::net::RtpHeaderExtension *extension) {
    // 判断是否带 RTP header extension
    const bool has_extension = extension != nullptr && !extension->payload.empty();

    // RTP 固定头第 0 字节：
    //
    // bit 布局：
    //   V V P X C C C C
    //
    // V：RTP version，这里是 2。
    // P：padding，这里不用，默认为 0。
    // X：extension，有扩展则置 1。
    // CC：CSRC count，这里不用 CSRC，默认为 0。
    buffer[0] = static_cast<std::uint8_t>((common::net::kRtpVersion << 6) | (has_extension ? 0x10 : 0x00));

    // RTP 固定头第 1 字节：
    //
    // bit 布局：
    //   M P P P P P P P
    //
    // M：marker bit。
    // PT：payload type，低 7 bit。
    buffer[1] = static_cast<std::uint8_t>((fields.marker ? 0x80 : 0x00) | (fields.payload_type & 0x7F));

    // 第 2~3 字节：sequence number，大端写入
    common::net::WriteBe16(fields.sequence_number, buffer + 2);

    // 第 4~7 字节：RTP timestamp，大端写入
    common::net::WriteBe32(fields.timestamp, buffer + 4);

    // 第 8~11 字节：SSRC，大端写入
    common::net::WriteBe32(fields.ssrc, buffer + 8);

    // 如果有扩展，继续写扩展头和扩展 payload
    if (has_extension) {
        // 扩展头前 2 字节：profile id
        common::net::WriteBe16(extension->profile_id, buffer + common::net::kRtpHeaderSize);

        // 扩展头后 2 字节：扩展 payload 长度。
        //
        // 注意：
        // RTP extension 长度单位不是字节，而是 32-bit word。
        // 所以这里是 payload.size() / 4。
        common::net::WriteBe16(
                static_cast<std::uint16_t>(extension->payload.size() / 4),
                buffer + common::net::kRtpHeaderSize + 2);

        // 拷贝扩展 payload
        std::copy(extension->payload.begin(), extension->payload.end(),
                  buffer + common::net::kRtpHeaderSize + 4);
    }
}

// 构建一个完整 RTP packet：
// [RTP Header][可选 RTP Extension][RTP Payload]
//
// 这个函数主要用于“单 NALU 直接打包”的场景。
void BuildRtpPacketInto(
        const common::net::RtpHeaderFields &header_fields,
        const common::net::RtpHeaderExtension *header_extension,
        const std::uint8_t *payload_data,
        std::size_t payload_size,
        std::vector<std::uint8_t> *output) {

    // 先写 RTP header。
    //
    // WriteRtpHeader 成功后，output 里已经有：[RTP Header][可选 Extension]
    if (!common::net::WriteRtpHeader(header_fields, header_extension, output)) {
        return;
    }

    // 再把 RTP payload 追加到后面。
    //
    // 对单 NALU 模式来说：payload_data 就是完整 NALU
    output->insert(output->end(), payload_data, payload_data + payload_size);
}

}  // namespace

// 构造 RTP 打包器
RtpPacketizer::RtpPacketizer(
        std::uint8_t payload_type,
        std::uint32_t clock_rate,
        std::size_t max_payload_size,
        std::uint32_t ssrc,
        bool enable_latency_extension)
        : payload_type_(payload_type),                  // 保存 RTP payload type。
          clock_rate_(clock_rate),                      // 保存 RTP clock rate。
          max_payload_size_(max_payload_size),          // 保存单包最大 payload。
          ssrc_(ssrc),                                  // 保存 SSRC。
          enable_latency_extension_(enable_latency_extension), // 是否开启延迟扩展。
          sequence_number_(BuildInitialSequenceNumber(ssrc)) { // 随机初始 sequence number。
}

// 把一帧 EncodedFrame 打包成多个 RTP 包。
bool RtpPacketizer::Packetize(
        common::model::EncodedFramePtr frame,
        std::vector<std::vector<std::uint8_t> > *packets,
        std::string *error_message) {

    // 参数检查：
    // frame 为空，没有数据可打包。
    // packets 为空，无法输出 RTP 包。
    if (!frame || packets == nullptr) {
        if (error_message != nullptr) {
            *error_message = "frame or packet output is null";
        }
        return false;
    }

    // 当前 RTP packetizer 只支持视频。
    //
    // 也就是说，它默认处理的是 H.264 视频帧。
    // 音频 RTP 打包通常是另一套规则。
    if (frame->type != common::model::StreamPayloadType::kVideo) {
        if (error_message != nullptr) {
            *error_message = "rtp packetizer currently supports video only";
        }
        return false;
    }

    // FU-A 至少需要额外 2 字节：
    // - FU indicator
    // - FU header
    //
    // 如果 max_payload_size_ <= 2，
    // 那连 FU-A 头都放不下，更别说放 H.264 数据了。
    if (max_payload_size_ <= 2) {
        if (error_message != nullptr) {
            *error_message = "rtp max payload size is too small";
        }
        return false;
    }

    // 把 Annex-B 格式的 H.264 payload 拆成多个 NALU。
    //
    // Annex-B 格式通常长这样：
    //
    // 00 00 00 01 [SPS]
    // 00 00 00 01 [PPS]
    // 00 00 00 01 [IDR]
    //
    // SplitAnnexBNalus 会得到：
    // nalus_[0] = SPS
    // nalus_[1] = PPS
    // nalus_[2] = IDR
    common::net::SplitAnnexBNalus(frame->payload, &nalus_);

    // 如果一个 NALU 都没拆出来，
    // 说明 frame payload 不是合法 Annex-B H.264 数据。
    if (nalus_.empty()) {
        if (error_message != nullptr) {
            *error_message = "frame payload does not contain Annex-B H264 NAL units";
        }
        return false;
    }

    // 记录 packets 原来的 capacity。
    //
    // 这是一个小优化：packets 可能之前已经分配过空间，
    // clear 后尽量复用这块内存，减少频繁分配。
    const std::size_t saved_capacity = packets->capacity();
    packets->clear();

    // 这里的判断通常不会触发，因为 clear 不会降低 capacity。
    // 保留这个写法是为了尽量保证 capacity 不变。
    if (saved_capacity > packets->capacity()) {
        packets->reserve(saved_capacity);
    }

    // 为当前帧生成 RTP timestamp。
    //
    // 同一帧拆出来的所有 RTP 包都使用同一个 timestamp。
    const std::uint32_t timestamp = BuildTimestamp(*frame);

    // 准备 RTP header extension。
    common::net::RtpHeaderExtension header_extension;

    // 默认没有扩展
    const common::net::RtpHeaderExtension *header_extension_ptr = nullptr;

    // 如果开启延迟扩展，就把采集时间戳放进 RTP 扩展
    if (enable_latency_extension_) {
        header_extension = common::net::BuildRtpLatencyHeaderExtension(
                common::net::RtpLatencyExtension{frame->capture_timestamp_ns, 0});
        header_extension_ptr = &header_extension;
    }

    // 遍历这一帧里的每一个 NALU。
    for (std::size_t index = 0; index < nalus_.size(); ++index) {
        // 当前 NALU
        const common::net::H264NaluView &nalu = nalus_[index];

        // marker 表示：这个 RTP 包是不是当前视频帧的最后一个 RTP 包。
        const bool marker = index + 1 == nalus_.size();

        // 如果当前 NALU 能放进一个 RTP payload，就用单 NALU 模式。
        if (nalu.size <= max_payload_size_) {
            if (!PacketizeSingleNalu(nalu.data, nalu.size, marker, timestamp, header_extension_ptr, packets)) {
                if (error_message != nullptr) {
                    *error_message = "failed to packetize single RTP/H264 NAL unit";
                }
                packets->clear();
                return false;
            }
            continue;
        }

        // 如果当前 NALU 太大，不能放进一个 RTP 包， 就用 FU-A 分片模式。
        if (!PacketizeFragmentedNalu(
                    nalu.data,
                    nalu.size,
                    marker,
                    timestamp,
                    header_extension_ptr,
                    packets,
                    error_message)) {
            packets->clear();
            return false;
        }
    }

    // 只要成功生成了至少一个 RTP 包，就返回 true
    return !packets->empty();
}

// 根据 EncodedFrame 构造 RTP timestamp。
std::uint32_t RtpPacketizer::BuildTimestamp(const common::model::EncodedFrame &frame) const {
    // 如果采集时间戳为 0，说明没有可靠采集时间。
    //
    // 这里退而求其次，用 frame.sequence 作为 timestamp。
    // 这不是标准媒体时间，但至少能保证不同帧有不同值。
    if (frame.capture_timestamp_ns == 0) {
        return static_cast<std::uint32_t>(frame.sequence & 0xFFFFFFFFu);
    }

    // 把纳秒采集时间转换成 RTP 媒体时钟。
    //
    // H.264 常用 clock_rate = 90000。
    //
    // 公式：
    // RTP timestamp = capture_timestamp_ns * clock_rate / 1,000,000,000
    const std::uint64_t timestamp =
            (frame.capture_timestamp_ns * static_cast<std::uint64_t>(clock_rate_)) / 1000000000ULL;

    // RTP timestamp 是 32 bit，会自然回绕
    return static_cast<std::uint32_t>(timestamp & 0xFFFFFFFFu);
}

// 单 NALU 打包。适用于 NALU 比较小，可以完整放入一个 RTP 包的情况
bool RtpPacketizer::PacketizeSingleNalu(
        const std::uint8_t *nalu_data,
        std::size_t nalu_size,
        bool marker,
        std::uint32_t timestamp,
        const common::net::RtpHeaderExtension *header_extension,
        std::vector<std::vector<std::uint8_t> > *packets) {
    // 准备 RTP 固定头字段
    common::net::RtpHeaderFields header_fields;
    header_fields.marker = marker;
    header_fields.payload_type = payload_type_;
    header_fields.sequence_number = sequence_number_++;
    header_fields.timestamp = timestamp;
    header_fields.ssrc = ssrc_;

    // 在 packets 末尾创建一个空 RTP packet。
    packets->emplace_back();

    // 取出刚创建的 packet
    std::vector<std::uint8_t> &packet = packets->back();

    // 构造完整 RTP 包：[RTP Header][可选 Extension][完整 NALU]
    BuildRtpPacketInto(header_fields, header_extension, nalu_data, nalu_size, &packet);
    if (packet.empty()) {
        packets->pop_back();
        return false;
    }
    return true;
}

// FU-A 分片打包：将超过 max_payload_size 的 NAL 单元拆分为多个 RTP packet
// 每个分片包含 FU indicator + FU header（标识 start/end 位和原始 NAL type）
bool RtpPacketizer::PacketizeFragmentedNalu(
        const std::uint8_t *nalu_data,
        std::size_t nalu_size,
        bool marker,
        std::uint32_t timestamp,
        const common::net::RtpHeaderExtension *header_extension,
        std::vector<std::vector<std::uint8_t> > *packets,
        std::string *error_message) {

    // NALU 至少要有 1 字节 NAL header，再加一点 payload 才值得分片。
    // nalu_size < 2 说明没有可分片的内容。
    if (nalu_data == nullptr || nalu_size < 2) {
        if (error_message != nullptr) {
            *error_message = "cannot fragment empty H264 NAL unit";
        }
        return false;
    }

    // FU-A RTP payload 格式：
    //
    // [FU indicator][FU header][NALU payload chunk]
    //
    // 前 2 字节是 FU-A 自己的头，所以真正能放原始 NALU 数据的容量是：
    // max_payload_size_ - 2
    const std::size_t fragment_payload_capacity = max_payload_size_ - 2;

    // 如果容量为 0，说明配置太小
    if (fragment_payload_capacity == 0) {
        if (error_message != nullptr) {
            *error_message = "rtp max payload size is too small for FU-A";
        }
        return false;
    }


    // H.264 NALU 第 1 个字节是 NALU header。
    //
    // bit 布局：
    //   F NRI NRI Type Type Type Type Type
    //
    // F：forbidden_zero_bit
    // NRI：重要性
    // Type：NALU 类型，比如 5=IDR，7=SPS，8=PPS
    const std::uint8_t nalu_header = nalu_data[0];

    // FU indicator 的格式和普通 NALU header 类似：
    //
    //   F NRI NRI Type Type Type Type Type
    //
    // 其中：
    // - F 和 NRI 继承原始 NALU header 的高 3 bit
    // - Type 改成 28，也就是 FU-A
    const std::uint8_t fu_indicator = static_cast<std::uint8_t>((nalu_header & 0xE0) | common::net::kH264FuAType);

    // 原始 NALU 类型。
    //
    // 低 5 bit 是 nal_type。
    //
    // 举例：
    // nalu_header = 0x65
    // 二进制：0110 0101
    // nal_type = 00101 = 5，也就是 IDR。
    const std::uint8_t nal_type = static_cast<std::uint8_t>(nalu_header & 0x1F);

    // FU header 的低 5 bit 要保存原始 nal_type
    const std::uint8_t fu_header_base = nal_type;

    // offset 从 1 开始。
    //
    // 为什么不是 0？因为 nalu_data[0] 是原始 NALU header。
    // FU-A 分片不会把原始 NALU header 直接放进 payload。
    //
    // 原始 NALU header 会被拆成：
    // - FU indicator
    // - FU header
    //
    // 所以真正切片的数据从 nalu_data[1] 开始。
    std::size_t offset = 1;

    // 不断切 NALU 剩余 payload
    while (offset < nalu_size) {
        // 剩余还没发送的字节数
        const std::size_t remaining = nalu_size - offset;

        // 当前 chunk 大小。
        //
        // 如果剩余很多，就取 fragment_payload_capacity。
        // 如果剩余不足一个满包，就取 remaining。
        const std::size_t chunk_size = std::min(fragment_payload_capacity, remaining);
        const bool start = offset == 1;
        const bool end = offset + chunk_size >= nalu_size;

        // 构造 FU header。
        //
        // FU header bit 布局：
        //   S E R Type Type Type Type Type
        //
        // S：Start，是否第一个分片。
        // E：End，是否最后一个分片。
        // R：Reserved，保留位，必须为 0。
        // Type：原始 NALU type。
        //
        // 0x80 = 1000 0000，设置 S 位。
        // 0x40 = 0100 0000，设置 E 位。
        const std::uint8_t fu_header = static_cast<std::uint8_t>(
                (start ? 0x80 : 0x00) | (end ? 0x40 : 0x00) | fu_header_base);

        // 准备 RTP header
        common::net::RtpHeaderFields header_fields;
        header_fields.marker = marker && end;
        header_fields.payload_type = payload_type_;
        header_fields.sequence_number = sequence_number_++;
        header_fields.timestamp = timestamp;
        header_fields.ssrc = ssrc_;

        packets->emplace_back();
        std::vector<std::uint8_t> &packet = packets->back();

        // 一次性构建 RTP header + FU indicator + FU header + NAL 数据块，减少拷贝次数
        // 判断是否有 RTP extension
        const bool has_ext = header_extension != nullptr && !header_extension->payload.empty();

        // 计算扩展区大小。 RTP extension 总大小 =4 字节 extension header + extension payload
        const std::size_t ext_size = has_ext ? (common::net::kRtpExtensionHeaderSize + header_extension->payload.size()) : 0;

        // RTP header 实际大小：12 字节固定 RTP header + 可选 extension。
        const std::size_t hdr_size = common::net::kRtpHeaderSize + ext_size;

        // 当前 RTP packet 总大小：
        //
        // [RTP Header + Extension]
        // [FU indicator 1 字节]
        // [FU header 1 字节]
        // [NALU chunk]
        const std::size_t total = hdr_size + 2 + chunk_size;
        packet.resize(total);

        // 写 RTP header 到 packet 开头
        WriteRtpHeaderToBuffer(packet.data(), header_fields, header_extension);

        // RTP payload 的第 1 字节：FU indicator
        packet[hdr_size] = fu_indicator;

        // RTP payload 的第 2 字节：FU header。
        packet[hdr_size + 1] = fu_header;

        // RTP payload 后续字节：原始 NALU 的一段数据。
        //
        // 注意：
        // 这里从 nalu_data + offset 开始，
        // 不包含 nalu_data[0] 的原始 NALU header。
        std::memcpy(packet.data() + hdr_size + 2, nalu_data + offset, chunk_size);

        if (packet.empty()) {
            packets->pop_back();
            if (error_message != nullptr) {
                *error_message = "failed to build RTP packet with latency extension";
            }
            return false;
        }

        // 移动 offset，准备处理下一个 chunk
        offset += chunk_size;
    }

    return true;
}

}  // namespace rtp
}  // namespace transport
}  // namespace modules
}  // namespace sserver
