#include "modules/network/StreamClientInternal.h"

#include <cerrno>

#include "common/log/Logger.h"
#include "common/net/H264AnnexB.h"
#include "common/net/RtpProtocol.h"

namespace sclient {

using common::net::AppendAnnexBNalu;
using common::net::AppendAnnexBStartCode;
using common::net::ParseRtpHeader;
using common::net::RtpHeaderFields;
using common::net::RtpHeaderExtension;
using common::net::RtpLatencyExtension;
using common::net::ParseRtpLatencyExtension;
using network_internal::BuildSocketError;
using network_internal::MonotonicNowNs;

// RTP 收一帧的主入口。
// 上层调用它时，希望拿到一个完整的 ReceivedFrame。
// 但 RTP 网络上来的是一个个 RTP packet，可能一个 packet 是完整 NALU，也可能是 FU-A 分片。
// 所以这里要循环接收 RTP 包，并把它们组装成一帧 H.264 Annex-B 数据。
bool StreamClient::ReceiveRtpFrame(ReceivedFrame *frame, std::string *error_message) {
    // 只要客户端还在运行，就一直尝试接收 RTP 包。
    while (running_.load()) {
        // 如果开启了 jitter buffer，就先尝试从 buffer 里取一帧现成的完整 RTP 帧。
        //
        // 注意：这里复用了 UDP jitter buffer 逻辑。
        // 因为 RTP 本质上也是跑在 UDP 上。
        if (config_.udp_jitter_buffer_enabled && TryPopReadyRtpFrame(frame)) {
            return true;
        }

        // 从 socket 接收一个 RTP datagram。
        //
        // RTP 通常跑在 UDP 上，所以一次 recv 通常就是一个 RTP 包。
        const ssize_t received = recv(socket_fd_, rtp_datagram_buffer_.data(), rtp_datagram_buffer_.size(), 0);

        if (received < 0) {
            if (errno == EINTR) {
                continue;
            }

            // 如果开启了 jitter buffer，即使本次 recv 出错，
            // 也再尝试从 buffer 里取一帧。
            //
            // 比如 socket 超时了，但 buffer 里可能已经有可播放帧。
            if (config_.udp_jitter_buffer_enabled) {
                ReceivedFrame buffered_frame;
                if (TryPopReadyRtpFrame(&buffered_frame)) {
                    // 如果取到了，就移动到输出参数 frame
                    *frame = std::move(buffered_frame);
                    return true;
                }
            }
            if (error_message != nullptr) {
                *error_message = BuildSocketError("failed to receive RTP datagram");
            }
            return false;
        }

        // 如果收到 0 字节 RTP 包
        if (received == 0) {
            common::log::Logger::Debug("received zero-length RTP datagram, ignoring");
            continue;
        }

        // 把 char* 转成 uint8_t*。
        // RTP 是二进制协议，用 uint8_t 表示字节更自然。
        const std::uint8_t *packet_data = reinterpret_cast<const std::uint8_t *>(rtp_datagram_buffer_.data());

        // RTP 包总长度
        const std::size_t packet_size = static_cast<std::size_t>(received);

        // 准备保存 RTP 头字段
        RtpHeaderFields header_fields;

        // 准备保存 RTP header extension
        RtpHeaderExtension header_extension;

        // header_size 表示 RTP 头部总长度。
        // 注意 RTP 头可能有 CSRC 和 extension，所以不一定只有固定 12 字节。
        std::size_t header_size = 0;

        // 解析 RTP header。
        //
        // 如果解析失败，或者整个 packet 只有 header 没有 payload，就忽略。
        if (!ParseRtpHeader(packet_data, packet_size, &header_fields, &header_size, &header_extension) ||
            packet_size <= header_size) {
            continue;
        }

        // 如果用户配置了 rtp_payload_type，就检查收到的 payload type 是否匹配。
        //
        // H.264 动态 payload type 常见是 96，但具体要看 SDP 或配置。
        if (config_.rtp_payload_type >= 0 && header_fields.payload_type != static_cast<std::uint8_t>(config_.rtp_payload_type)) {
            continue;
        }

        // 判断当前 RTP 包是否属于一个新的 RTP 帧。
        //
        // RTP 里同一帧拆出来的多个 RTP 包通常 timestamp 相同。
        // SSRC 表示流 ID。
        //
        // 如果：
        // 1. 当前没有 active 的组帧状态
        // 2. timestamp 变了
        // 3. ssrc 变了
        //
        // 都说明要开始组装一帧新的 H.264 数据。
        if (!rtp_frame_assembly_.active ||
            rtp_frame_assembly_.timestamp != header_fields.timestamp ||
            rtp_frame_assembly_.ssrc != header_fields.ssrc) {
            rtp_frame_assembly_.payload.clear();
            rtp_frame_assembly_.timestamp = header_fields.timestamp;
            rtp_frame_assembly_.ssrc = header_fields.ssrc;
            rtp_frame_assembly_.next_sequence_number = static_cast<std::uint16_t>(header_fields.sequence_number + 1);
            rtp_frame_assembly_.active = true;
            rtp_frame_assembly_.has_sequence_number = true;
            rtp_frame_assembly_.capture_timestamp_ns = 0;
            rtp_frame_assembly_.transport_send_timestamp_ns = 0;
            rtp_frame_assembly_.sender_metadata_available = false;
            rtp_frame_assembly_.sender_metadata_invalid = false;
            rtp_frame_assembly_.frame_damaged = false;
            rtp_frame_assembly_.fu_in_progress = false;

        // 如果不是新帧，并且当前 RTP 包的 sequence number 不是期望值。
        } else if (rtp_frame_assembly_.has_sequence_number &&
                   header_fields.sequence_number != rtp_frame_assembly_.next_sequence_number) {
            rtp_frame_assembly_.frame_damaged = true;
            rtp_frame_assembly_.fu_in_progress = false;
            rtp_frame_assembly_.next_sequence_number = static_cast<std::uint16_t>(header_fields.sequence_number + 1);

        // 否则 sequence number 正常连续
        } else {
            rtp_frame_assembly_.next_sequence_number = static_cast<std::uint16_t>(header_fields.sequence_number + 1);
            rtp_frame_assembly_.has_sequence_number = true;
        }

        // 如果配置要求接收 metadata，并且当前帧的 metadata 还没有被判定为非法。
        if (config_.expect_metadata && !rtp_frame_assembly_.sender_metadata_invalid) {
            RtpLatencyExtension latency_extension;

            // 尝试从 RTP header extension 中解析延迟信息。
            //
            // 如果解析失败，或者关键时间戳为 0，就认为 metadata 不可用。
            if (!ParseRtpLatencyExtension(header_extension, &latency_extension) ||
                latency_extension.capture_timestamp_ns == 0 ||
                latency_extension.transport_send_timestamp_ns == 0) {
                rtp_frame_assembly_.sender_metadata_available = false;
                rtp_frame_assembly_.sender_metadata_invalid = true;

            // 如果之前还没有保存过 metadata
            } else if (!rtp_frame_assembly_.sender_metadata_available) {
                rtp_frame_assembly_.capture_timestamp_ns = latency_extension.capture_timestamp_ns;
                rtp_frame_assembly_.transport_send_timestamp_ns = latency_extension.transport_send_timestamp_ns;
                rtp_frame_assembly_.sender_metadata_available = true;

            // 如果同一帧后续 RTP 包里的 capture timestamp 和之前不一致
            } else if (rtp_frame_assembly_.capture_timestamp_ns != latency_extension.capture_timestamp_ns) {
                rtp_frame_assembly_.sender_metadata_available = false;
                rtp_frame_assembly_.sender_metadata_invalid = true;

            // 否则 metadata 是一致的
            } else {
                rtp_frame_assembly_.transport_send_timestamp_ns = latency_extension.transport_send_timestamp_ns;
            }
        }

        // RTP payload 起始位置 = packet_data + RTP header 总长度。
        const std::uint8_t *payload_data = packet_data + header_size;

        // RTP payload 大小 = packet 总长度 - header 长度。
        const std::size_t payload_size = packet_size - header_size;
        if (payload_size > 0) {
            // H.264 NALU 第一个字节低 5 bit 是 nal_type
            const std::uint8_t nal_type = static_cast<std::uint8_t>(payload_data[0] & 0x1F);

            // nal_type 1~23 表示单 NALU 包
            if (nal_type > 0 && nal_type < 24) {
                // 把这个完整 NALU 追加到 Annex-B payload。
                //
                // 结果类似：
                // 00 00 00 01 [NALU bytes]
                AppendAnnexBNalu(payload_data, payload_size, &rtp_frame_assembly_.payload);

                // 单 NALU 不涉及 FU-A 分片，所以标记没有 FU 正在进行
                rtp_frame_assembly_.fu_in_progress = false;

            // nal_type == 28 表示 H.264 FU-A 分片包。
            //
            // FU-A 用于把一个很大的 NALU 拆成多个 RTP 包。
            // payload_size >= 2 是因为 FU-A 至少要有：
            // payload_data[0] = FU indicator
            // payload_data[1] = FU header
            } else if (nal_type == common::net::kH264FuAType && payload_size >= 2) {
                // FU header 是 FU-A payload 的第 2 个字节
                const std::uint8_t fu_header = payload_data[1];

                // FU header 最高 bit 是 Start 位。
                // start=true 表示这是一个大 NALU 的第一个 FU-A 分片。
                const bool start = (fu_header & 0x80) != 0;

                // FU header 第二高 bit 是 End 位。
                // end=true 表示这是一个大 NALU 的最后一个 FU-A 分片。
                const bool end = (fu_header & 0x40) != 0;

                // 重建原始 NALU header。
                //
                // FU-A 中：
                // payload_data[0] 是 FU indicator，保留了 F 和 NRI 位。
                // fu_header 低 5 bit 保存原始 nal_type。
                //
                // 所以原始 NALU header =
                // FU indicator 的高 3 bit | FU header 的低 5 bit。
                const std::uint8_t reconstructed_nal_header =
                        static_cast<std::uint8_t>((payload_data[0] & 0xE0) | (fu_header & 0x1F));

                // 如果这是 FU-A 的开始分片
                if (start) {
                    // 如果 payload_size <= 2，说明除了 FU indicator 和 FU header，
                    // 后面没有真正的 NALU 数据
                    if (payload_size <= 2) {
                        rtp_frame_assembly_.frame_damaged = true;
                        rtp_frame_assembly_.fu_in_progress = false;

                    // 正常的 FU-A 起始分片
                    } else {
                        // 先写 Annex-B start code。
                        //
                        // 因为最终要恢复成：
                        // 00 00 00 01 [reconstructed_nal_header] [NALU body...]
                        AppendAnnexBStartCode(&rtp_frame_assembly_.payload);

                        // 写入重建出来的原始 NALU header
                        rtp_frame_assembly_.payload.push_back(reconstructed_nal_header);

                        // 写入当前 FU-A 分片中的 NALU 数据部分。
                        //
                        // 注意 payload_data + 2 才是真正数据：
                        // payload_data[0] 是 FU indicator
                        // payload_data[1] 是 FU header
                        rtp_frame_assembly_.payload.insert(
                                rtp_frame_assembly_.payload.end(),
                                payload_data + 2,
                                payload_data + payload_size);

                        // 如果 start 包同时也是 end 包，说明一个 FU-A 就结束了。
                        // 一般大 NALU 不会这样，但代码兼容。
                        // 如果不是 end，说明后面还要继续接 FU-A 中间分片。
                        rtp_frame_assembly_.fu_in_progress = !end;
                    }

                // 如果不是 start 分片
                } else if (!rtp_frame_assembly_.fu_in_progress || payload_size <= 2) {
                    // 走到这里表示异常：
                    //
                    // 1. 当前不是 start，但是之前没有 FU-A 正在进行。
                    //    说明中间分片或结束分片先来了，缺了 start。
                    //
                    // 2. payload_size <= 2，没有真正数据。
                    //
                    // 这两种都会导致当前帧不完整。
                    rtp_frame_assembly_.frame_damaged = true;
                    rtp_frame_assembly_.fu_in_progress = false;

                // 正常的 FU-A 中间分片或结束分片
                } else {
                    // 追加当前 FU-A 分片的实际数据部分
                    rtp_frame_assembly_.payload.insert(
                            rtp_frame_assembly_.payload.end(),
                            payload_data + 2,
                            payload_data + payload_size);

                    // 如果这是 end 分片
                    if (end) {
                        rtp_frame_assembly_.fu_in_progress = false;
                    }
                }

            // 其他 nal_type 当前不支持
            } else {
                rtp_frame_assembly_.frame_damaged = true;
                rtp_frame_assembly_.fu_in_progress = false;
            }
        }

        // RTP marker bit 通常表示“一帧的最后一个 RTP 包”。
        //
        // 如果 marker=false，说明这一帧还没结束，
        // 继续接收下一个 RTP 包。
        if (!header_fields.marker) {
            continue;
        }

        // 走到这里说明 marker=true。
        // 也就是服务端告诉客户端：当前 RTP 包是一帧的最后一个包
        const std::uint64_t now_ns = MonotonicNowNs();

        // 判断当前组装出来的 RTP 帧是否可用
        const bool frame_ready =
                rtp_frame_assembly_.active &&
                !rtp_frame_assembly_.payload.empty() &&
                !rtp_frame_assembly_.frame_damaged &&
                !rtp_frame_assembly_.fu_in_progress;

        // 如果这一帧可用。
        if (frame_ready) {
            ReceivedFrame completed_frame;
            std::memset(&completed_frame.header, 0, sizeof(completed_frame.header));
            protocol::FillMessageMagic(completed_frame.header.head_id);
            completed_frame.header.message_type = static_cast<std::uint16_t>(protocol::MessageType::kAvStream);
            completed_frame.header.payload_length = static_cast<std::uint32_t>(rtp_frame_assembly_.payload.size());
            std::memset(&completed_frame.metadata, 0, sizeof(completed_frame.metadata));
            completed_frame.metadata.sequence = static_cast<std::uint64_t>(rtp_frame_assembly_.timestamp);
            completed_frame.sender_metadata_available = false;

            // 如果配置要求 metadata，并且 RTP header extension 中的 metadata 可用。
            if (config_.expect_metadata &&
                rtp_frame_assembly_.sender_metadata_available &&
                !rtp_frame_assembly_.sender_metadata_invalid) {
                completed_frame.metadata.capture_timestamp_ns = rtp_frame_assembly_.capture_timestamp_ns;
                completed_frame.metadata.transport_send_timestamp_ns = rtp_frame_assembly_.transport_send_timestamp_ns;
                completed_frame.sender_metadata_available = true;
            }
            completed_frame.receive_timestamp_ns = now_ns;
            completed_frame.payload = rtp_frame_assembly_.payload;

            // 如果开启了 jitter buffer。
            if (config_.udp_jitter_buffer_enabled) {
                // 更新 UDP/RTP 共用的完成帧统计。
                //
                // 虽然函数叫 OnCompletedUdpFrame，
                // 但 RTP 也是基于 UDP，且复用了同一套统计逻辑。
                OnCompletedUdpFrame(completed_frame, now_ns);

                // 把完整帧放入 jitter buffer
                BufferCompletedUdpFrame(std::move(completed_frame));

                // 重置 RTP 组帧状态，准备接收下一帧。
                ResetRtpState();
                continue;
            }

            // 如果没有开启 jitter buffer，
            // 就直接把完整帧移动到输出参数 frame。
            *frame = std::move(completed_frame);
        }

        // 不管 frame_ready 是否为 true，
        // marker 到来后当前 RTP 帧都结束了，需要重置组帧状态。
        ResetRtpState();

        // 如果刚才确实组出了一帧完整可用帧。
        if (frame_ready) {
            return true;
        }
    }

    // 如果 running_ 变成 false，说明客户端正在停止
    if (error_message != nullptr) {
        *error_message = "client is stopping";
    }
    return false;
}

// 尝试从 RTP jitter buffer 中取出一帧。
// 实际上它复用了 UDP jitter buffer 的取帧逻辑。
bool StreamClient::TryPopReadyRtpFrame(ReceivedFrame *frame) {
    if (frame == nullptr || udp_jitter_buffer_.empty()) {
        return false;
    }

    const std::uint64_t now_ns = MonotonicNowNs();

    // 复用 UDP 的 TryPopReadyUdpFrame。
    //
    // 因为 RTP 和 UDP 自定义协议最终都会变成 ReceivedFrame，
    // jitter buffer 只关心 frame.metadata.sequence 和 receive_timestamp_ns，
    // 不关心它原来是 RTP 还是自定义 UDP。
    return TryPopReadyUdpFrame(frame, now_ns);
}

}  // namespace sclient
