#include "modules/network/StreamClientInternal.h"

#include <algorithm>
#include <cmath>

namespace sclient {

using network_internal::BuildSocketError;
using network_internal::MonotonicNowNs;
using network_internal::XorInto;
using network_internal::kRecentCompletedUdpSequenceWindow;
using network_internal::kUdpAssemblyTimeoutNs;

// UDP 收一帧的主入口。
// 上层 main.cpp / 网络线程会调用这个函数，希望拿到一帧完整的 EncodedFrame。
// 注意：UDP 发来的不是完整帧，而是一个个 fragment，所以这里要不断收包、重组、纠错、NACK、抖动缓冲。
bool StreamClient::ReceiveUdpFrame(ReceivedFrame *frame, std::string *error_message) {
    while (running_.load()) {
        // 记录本轮循环开始时的当前时间，单位纳秒。
        const std::uint64_t loop_now_ns = MonotonicNowNs();

        // 先尝试从 jitter buffer 里取出一帧已经准备好的完整帧。
        // 也就是说：可能之前已经收到完整帧，只是为了平滑播放，先放在缓冲区里等一等。
        if (TryPopReadyUdpFrame(frame, loop_now_ns)) {
            return true;
        }

        // 标记本轮是否从 socket 里“快速抽干”过 UDP 包。
        bool drained_any_datagram = false;

        // 进入一个内部循环：尽可能把当前 socket 里已经到达的 UDP 包都读出来
        while (running_.load()) {
            // 使用 recv 从 UDP socket 收一个 datagram。
            // MSG_DONTWAIT 表示非阻塞读取：如果现在没包，不要卡住，立刻返回。
            const ssize_t received = recv(socket_fd_, udp_datagram_buffer_.data(), udp_datagram_buffer_.size(), MSG_DONTWAIT);
            if (received < 0) {
                if (errno == EAGAIN || errno == EWOULDBLOCK) {
                    break;
                }
                if (errno == EINTR) {
                    continue;
                }
                if (error_message != nullptr) {
                    *error_message = BuildSocketError("failed to receive UDP datagram");
                }
                return false;
            }
            if (received == 0) {
                if (error_message != nullptr) {
                    *error_message = "UDP socket closed";
                }
                return false;
            }

            // 走到这里说明真的收到一个 UDP datagram
            drained_any_datagram = true;
            bool frame_ready = false;

            // 处理这个 UDP datagram。
            // 它可能是普通数据分片，也可能是 FEC 校验分片。
            // ProcessUdpDatagram 内部会做协议校验、分片保存、FEC 恢复、完整帧判断。
            if (!ProcessUdpDatagram(
                        udp_datagram_buffer_.data(),          // datagram 原始字节地址
                        static_cast<std::size_t>(received),   // datagram 字节长度
                        MonotonicNowNs(),                     // 当前接收时间
                        true,                                 // defer_ready_pop=true：先延迟从 jitter buffer 弹出
                        frame,                                // 如果完成一帧，写到 frame
                        &frame_ready)) {                      // 输出：是否得到完整帧
                // 如果这个 datagram 无效，或者暂时没拼完整，就继续收下一个 UDP 包。
                continue;
            }

            // 如果处理这个 datagram 后，已经得到完整帧
            if (frame_ready) {
                return true;
            }
        }

        // 记录抽干 socket 之后的当前时间
        const std::uint64_t after_drain_now_ns = MonotonicNowNs();

        // 尝试用 FEC 恢复一些“只缺一个分片”的帧。
        if (FinalizeRecoverableUdpFecFrames(after_drain_now_ns, true, frame)) {
            return true;
        }

        // 再尝试从 jitter buffer 里取一帧。
        // 因为刚才 FEC 恢复可能已经把完整帧放进 jitter buffer 了。
        if (TryPopReadyUdpFrame(frame, after_drain_now_ns)) {
            return true;
        }

        // 如果刚才确实收到过 UDP 包，那本轮已经做了不少工作
        if (drained_any_datagram) {
            continue;
        }

        // 如果本轮没有收到任何 UDP 包，就清理超时的半成品帧。
        PruneExpiredUdpAssemblies(after_drain_now_ns);

        // 清理之后，再尝试用 FEC 恢复。
        if (FinalizeRecoverableUdpFecFrames(after_drain_now_ns, false, frame)) {
            return true;
        }

        // 对仍然缺分片的帧，检查是否需要发送 NACK。
        MaybeSendUdpNackRequests(after_drain_now_ns);

        // 发送 NACK 后，再试着从 jitter buffer 取帧。
        if (TryPopReadyUdpFrame(frame, after_drain_now_ns)) {
            return true;
        }

        // 到这里说明：
        // 1. 当前没有现成完整帧；
        // 2. socket 也暂时没包；
        // 3. FEC/NACK/超时清理都处理过了。
        // 于是进入阻塞 recv，等待下一个 UDP datagram 到来。
        const ssize_t received = recv(socket_fd_, udp_datagram_buffer_.data(), udp_datagram_buffer_.size(), 0);
        if (received < 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR) {
                continue;
            }
            if (error_message != nullptr) {
                *error_message = BuildSocketError("failed to receive UDP datagram");
            }
            return false;
        }
        if (received == 0) {
            if (error_message != nullptr) {
                *error_message = "UDP socket closed";
            }
            return false;
        }

        // 标记这个 datagram 是否让我们拼出完整
        bool frame_ready = false;
        // 处理刚刚阻塞等到的 UDP datagram。
        if (!ProcessUdpDatagram(
                    udp_datagram_buffer_.data(),          // datagram 数据
                    static_cast<std::size_t>(received),   // datagram 长度
                    MonotonicNowNs(),                     // 接收时间
                    false,                                // 不延迟弹出，能出帧就尽量出帧
                    frame,                                // 输出完整帧
                    &frame_ready)) {                      // 输出是否完成
            // 如果这个包无效，或者没拼出完整帧，就继续下一轮。
            continue;
        }

        // 如果已经得到完整帧
        if (frame_ready) {
            return true;
        }
    }

    // 如果 running_ 变成 false，说明客户端被要求停止。
    if (error_message != nullptr) {
        *error_message = "client stopped";
    }
    return false;
}

// 处理一个 UDP datagram。
// 注意：UDP 是“一个包一个包”来的，这里的 datagram 就是一次 recv 收到的完整 UDP 包。
// 它可能是一帧视频的某个数据分片，也可能是 FEC 校验分片。
bool StreamClient::ProcessUdpDatagram(
        const char *data,
        std::size_t datagram_size,
        std::uint64_t now_ns,
        bool defer_ready_pop,          // 是否延迟从 jitter buffer 取帧
        ReceivedFrame *frame,          // 如果拼出完整帧，就写到这里
        bool *frame_ready) {           // 输出参数：是否已经得到完整帧

    // 创建一个统计快照保护对象。
    // 这个对象析构时会把 udp_receive_stats_ 发布成 snapshot，方便 HUD 显示最新统计。
    UdpStatsSnapshotGuard snapshot_guard(this);

    if (frame_ready != nullptr) {
        *frame_ready = false;
    }

    // UDP 收到的 datagram 数量 +1
    ++udp_receive_stats_.datagrams_received;

    // 每次处理新包前，顺手清理已经超时的半成品帧。
    // 比如某一帧只收到 3/5 个分片，超过一段时间还没补齐，就丢掉。
    PruneExpiredUdpAssemblies(now_ns);

    // 一个合法的 UDP 视频分片包，至少要包含：
    // 1. 项目自定义 MessageHeader
    // 2. UDP 分片头 UdpFrameFragmentHeader
    //
    // 如果连这两个头都不够，说明包太短，肯定非法。
    if (datagram_size < sizeof(protocol::MessageHeader) + sizeof(protocol::UdpFrameFragmentHeader)) {
        ++udp_receive_stats_.invalid_datagrams;
        return false;
    }

    protocol::MessageHeader header{};
    std::memcpy(&header, data, sizeof(header));
    if (!protocol::HasValidMessageMagic(header)) {
        ++udp_receive_stats_.invalid_datagrams;
        return false;
    }

    // 检查消息类型和 payload 长度。
    if (header.message_type != static_cast<std::uint16_t>(protocol::MessageType::kAvStream) ||
        header.payload_length < sizeof(protocol::UdpFrameFragmentHeader)) {
        ++udp_receive_stats_.invalid_datagrams;
        return false;
    }

    // 准备 UDP 分片头结构体。
    // 这个头里会告诉我们：
    // 这是第几帧、这一帧共有几个分片、当前是第几个分片、分片偏移是多少等。
    protocol::UdpFrameFragmentHeader fragment_header{};
    std::memcpy(&fragment_header, data + sizeof(header), sizeof(fragment_header));
    if (fragment_header.fragment_count == 0 ||
        fragment_header.fragment_role > static_cast<std::uint16_t>(protocol::UdpFragmentRole::kXorParity) ||
        (fragment_header.fragment_role == static_cast<std::uint16_t>(protocol::UdpFragmentRole::kData) &&
         fragment_header.fragment_index >= fragment_header.fragment_count)) {
        ++udp_receive_stats_.invalid_datagrams;
        return false;
    }

    // 计算真正的视频分片 payload 大小。
    const std::size_t fragment_payload_size = datagram_size - sizeof(header) - sizeof(fragment_header);

    // 校验 header.payload_length 是否和实际 datagram 长度一致
    if (fragment_payload_size + sizeof(fragment_header) != header.payload_length) {
        ++udp_receive_stats_.invalid_datagrams;
        return false;
    }

    // 如果这是普通数据分片，要检查它拷贝到整帧 payload 里时不会越界。
    // 例如整帧只有 10000 字节，但这个分片说自己从 9900 开始，长度 500，
    // 那就会写到 10400，越界了，必须丢弃。
    if (fragment_header.fragment_role == static_cast<std::uint16_t>(protocol::UdpFragmentRole::kData) &&
        fragment_header.fragment_offset + fragment_payload_size > fragment_header.frame_payload_size) {
        ++udp_receive_stats_.invalid_datagrams;
        return false;
    }

    // 测试用：根据配置模拟 UDP 丢包。
    // 比如你启动客户端时配置了测试丢包模式，这里会故意丢掉某些分片。
    if (ShouldInjectUdpLoss(fragment_header)) {
        return false;
    }

    // 判断这个帧序号是不是最近已经完成过。
    //
    // UDP 可能乱序，也可能服务端 NACK 重传导致旧分片迟到。
    // 如果这一帧已经完整交给解码器了，后面迟到的分片就没有意义。
    if (IsRecentlyCompletedUdpFrameSequence(fragment_header.frame_sequence)) {
        ++udp_receive_stats_.stale_datagrams_dropped;
        return false;
    }

    // 根据 frame_sequence 找到这一帧对应的重组对象。
    //
    // udp_assemblies_ 可以理解成：
    // map<帧序号, 这一帧当前收到的所有分片状态>
    //
    // 如果这个 frame_sequence 之前不存在，operator[] 会自动创建一个新的 UdpFrameAssembly。
    UdpFrameAssembly &assembly = udp_assemblies_[fragment_header.frame_sequence];

    // 如果 received_fragments 为空，说明这是第一次看到这一帧。
    if (assembly.received_fragments.empty()) {
        assembly.header = header;
        assembly.metadata.sequence = fragment_header.frame_sequence;
        assembly.metadata.capture_timestamp_ns = config_.expect_metadata ? fragment_header.capture_timestamp_ns : 0;
        assembly.metadata.encode_start_timestamp_ns = config_.expect_metadata ? fragment_header.encode_start_timestamp_ns : 0;
        assembly.metadata.encode_end_timestamp_ns = config_.expect_metadata ? fragment_header.encode_end_timestamp_ns : 0;
        assembly.metadata.transport_send_timestamp_ns = config_.expect_metadata ? fragment_header.transport_send_timestamp_ns : 0;
        assembly.payload.resize(fragment_header.frame_payload_size);
        assembly.received_fragments.assign(fragment_header.fragment_count, false);
        assembly.fragment_offsets.assign(fragment_header.fragment_count, 0);
        assembly.fragment_payload_sizes.assign(fragment_header.fragment_count, 0);
        assembly.first_seen_timestamp_ns = now_ns;
        assembly.last_fragment_timestamp_ns = now_ns;
    }

    // 检查当前分片描述的 frame 结构，是否和之前已经创建的 assembly 一致。
    //
    // 比如第一次看到这一帧时说 fragment_count=5；
    // 结果后面同一 frame_sequence 又来了一个包说 fragment_count=6；
    // 这种就说明协议数据异常，不能继续拼。
    if (assembly.received_fragments.size() != fragment_header.fragment_count ||
        assembly.payload.size() != fragment_header.frame_payload_size) {
        ++udp_receive_stats_.timed_out_frames;
        udp_receive_stats_.timed_out_fragments +=
                static_cast<std::uint64_t>(assembly.received_fragments.size() - assembly.received_fragment_count);
        udp_test_dropped_fragments_.erase(fragment_header.frame_sequence);
        udp_assemblies_.erase(fragment_header.frame_sequence);
        return false;
    }

    // 如果当前包是 XOR FEC 校验分片。
    if (fragment_header.fragment_role == static_cast<std::uint16_t>(protocol::UdpFragmentRole::kXorParity)) {
        // 只有配置开启 UDP FEC 时，才保存这个校验分片
        if (config_.udp_fec_enabled) {
            // 把 FEC payload 拷贝到 assembly.fec_payload
            assembly.fec_payload.assign(
                    reinterpret_cast<const std::uint8_t *>(data + sizeof(header) + sizeof(fragment_header)),
                    reinterpret_cast<const std::uint8_t *>(data + datagram_size));
            assembly.has_fec_payload = true;
            assembly.fec_payload_timestamp_ns = now_ns;
            ++udp_receive_stats_.fec_fragments_received;
        }
        assembly.last_fragment_timestamp_ns = now_ns;
    } else {
        // 走到这里说明当前包是普通数据分片
        ++udp_receive_stats_.fragments_received;

        // 如果这个 fragment_index 之前没收到过
        if (!assembly.received_fragments[fragment_header.fragment_index]) {
            // 把当前分片 payload 拷贝到完整帧 payload 的正确位置。
            //
            // 举例：
            // 完整帧大小 8000 字节
            // fragment_index = 2
            // fragment_offset = 2800
            // fragment_payload_size = 1400
            //
            // 那就把这 1400 字节拷贝到 assembly.payload[2800..4199]。
            std::memcpy(
                    assembly.payload.data() + fragment_header.fragment_offset,
                    data + sizeof(header) + sizeof(fragment_header),
                    fragment_payload_size);
            assembly.received_fragments[fragment_header.fragment_index] = true;
            assembly.fragment_offsets[fragment_header.fragment_index] = fragment_header.fragment_offset;
            assembly.fragment_payload_sizes[fragment_header.fragment_index] =
                    static_cast<std::uint32_t>(fragment_payload_size);
            ++assembly.received_fragment_count;
            assembly.last_fragment_timestamp_ns = now_ns;
        } else {

            // 如果这个 fragment_index 已经收到过，说明这是重复分片。
            //
            // 可能原因：
            // 1. 网络重复投递
            // 2. 服务端收到 NACK 后重传，但原始包后来也到了
            // 3. 客户端重复收到同一个包
            ++udp_receive_stats_.duplicate_fragments;
        }
    }

    // 如果当前包是普通数据分片，并且这一帧还没收齐，就尝试用 FEC 恢复缺失分片。
    if (fragment_header.fragment_role == static_cast<std::uint16_t>(protocol::UdpFragmentRole::kData) &&
        assembly.received_fragment_count < assembly.received_fragments.size()) {
        MaybeRecoverWithUdpFec(&assembly);
    }

    // 如果这一帧的所有分片都收齐了，就把 assembly 转成完整 ReceivedFrame。
    if (assembly.received_fragment_count == assembly.received_fragments.size() &&
        FinalizeCompletedUdpFrame(fragment_header.frame_sequence, &assembly, now_ns, defer_ready_pop, frame)) {
        if (frame_ready != nullptr) {
            *frame_ready = true;
        }
        return true;
    }

    // 走到这里说明：
    // 当前 datagram 处理成功，但还没有得到完整帧
    return false;
}

// 尝试把所有“可以通过 FEC 恢复”的 UDP 帧恢复出来。
//
// 什么时候能恢复？
// 一帧被拆成多个数据分片，同时服务端额外发送一个 XOR 校验分片。
// 如果这一帧只丢了 1 个数据分片，就可以通过 FEC 恢复。
// 如果丢了 2 个或更多，一般这个 XOR FEC 就恢复不了了。
bool StreamClient::FinalizeRecoverableUdpFecFrames(std::uint64_t now_ns, bool defer_ready_pop, ReceivedFrame *frame) {
    // 创建统计快照保护对象
    UdpStatsSnapshotGuard snapshot_guard(this);

    // 保存“刚刚通过 FEC 恢复成功的帧序号”
    std::vector<std::uint64_t> recovered_sequences;

    // 遍历当前所有还在重组中的 UDP 帧。
    // udp_assemblies_ 的结构可以理解成：
    // map<frame_sequence, UdpFrameAssembly>
    //
    // 例如：
    // 100 -> 第 100 帧的分片收集状态
    // 101 -> 第 101 帧的分片收集状态
    // 102 -> 第 102 帧的分片收集状态
    for (std::map<std::uint64_t, UdpFrameAssembly>::iterator it = udp_assemblies_.begin();
         it != udp_assemblies_.end();
         ++it) {
        UdpFrameAssembly &assembly = it->second;

        // 如果这一帧已经收齐了，就不需要 FEC 恢复
        if (assembly.received_fragment_count >= assembly.received_fragments.size()) {
            continue;
        }

        // 尝试用 FEC 恢复这一帧。
        if (MaybeRecoverWithUdpFec(&assembly)) {
            recovered_sequences.push_back(it->first);
        }
    }

    // 遍历刚刚恢复成功的帧序号
    for (std::size_t index = 0; index < recovered_sequences.size(); ++index) {
        // 取出第 index 个恢复成功的帧序号
        const std::uint64_t sequence = recovered_sequences[index];

        // 根据 sequence 再去 udp_assemblies_ 里找对应 assembly。
        std::map<std::uint64_t, UdpFrameAssembly>::iterator it = udp_assemblies_.find(sequence);

        // 如果找不到，说明它可能已经被别的逻辑清理掉了
        if (it == udp_assemblies_.end()) {
            continue;
        }

        // 如果恢复后仍然没有收齐所有分片，就不能 finalize
        if (it->second.received_fragment_count != it->second.received_fragments.size()) {
            continue;
        }

        // 到这里说明：这一帧现在已经完整了。
        // 把 UdpFrameAssembly 转成 ReceivedFrame。
        if (FinalizeCompletedUdpFrame(sequence, &it->second, now_ns, defer_ready_pop, frame)) {
            return true;
        }
    }

    return false;
}

// 尝试从 UDP jitter buffer 中取出一帧可以播放/解码的完整帧。
//
// jitter buffer 的作用：
// UDP 包到达时间可能忽快忽慢，有的帧先到，有的帧后到。
// 如果一收到完整帧就立刻交给解码器，播放可能会一卡一卡。
// 所以可以把完整帧先放进 jitter buffer，稍微等一等，让乱序和抖动被平滑掉。
bool StreamClient::TryPopReadyUdpFrame(ReceivedFrame *frame, std::uint64_t now_ns) {
    UdpStatsSnapshotGuard snapshot_guard(this);
    if (frame == nullptr || !config_.udp_jitter_buffer_enabled || udp_jitter_buffer_.empty()) {
        return false;
    }

    // 获取当前 jitter buffer 的目标等待时间。
    //
    // 比如 target_delay_ns = 30ms，
    // 那一帧进入 jitter buffer 后，至少等大约 30ms 才考虑输出。
    const std::uint64_t target_delay_ns = CurrentJitterBufferTargetDelayNs();

    // 读取 jitter buffer 策略。
    //
    // 可能的模式包括：
    // auto、off、low、smooth、adaptive 等。
    const std::string &mode = config_.udp_jitter_buffer_strategy;
    const bool use_adaptive_params = (mode == "auto" || mode == "off" || mode == "low" || mode == "smooth");

    // 计算实际最大等待时间，单位 ms
    const int effective_max_wait_ms = use_adaptive_params
            ? adaptive_jitter_.quality_max_wait_ms()
            : config_.udp_jitter_buffer_max_wait_ms;

    // 计算 jitter buffer 最多允许缓存多少帧
    const std::size_t effective_max_frames = use_adaptive_params
            ? adaptive_jitter_.quality_max_frames()
            : config_.udp_jitter_buffer_max_frames;

    // 把最大等待时间从 ms 转成 ns
    const std::uint64_t max_wait_ns =
            static_cast<std::uint64_t>(std::max(effective_max_wait_ms, 0)) * 1000ULL * 1000ULL;

    // udp_jitter_buffer_ 是 map，key 是 frame sequence。
    // begin() 拿到的是当前缓存中序号最小，也就是最老的一帧。
    std::map<std::uint64_t, BufferedUdpFrame>::iterator oldest = udp_jitter_buffer_.begin();

    // 如果还没有初始化“下一帧期望输出的序号”。
    if (!has_next_jitter_buffer_sequence_) {
        // 就把当前最老帧的序号作为下一帧期望序号
        next_jitter_buffer_sequence_ = oldest->first;
        has_next_jitter_buffer_sequence_ = true;
    }

    // 尝试在 jitter buffer 中查找“下一帧应该输出的序号”
    std::map<std::uint64_t, BufferedUdpFrame>::iterator expected = udp_jitter_buffer_.find(next_jitter_buffer_sequence_);

    // 如果找不到 expected，说明中间可能缺帧了。
    //
    // 举例：
    // next_jitter_buffer_sequence_ = 100
    // 但 buffer 里现在只有 101、102
    // 说明第 100 帧还没来，或者已经丢了。
    if (expected == udp_jitter_buffer_.end()) {
        // 计算当前最老帧已经在 buffer 里等待了多久
        const std::uint64_t oldest_wait_ns = now_ns > oldest->second.buffered_timestamp_ns
                ? now_ns - oldest->second.buffered_timestamp_ns
                : 0;

        // 判断是否应该跳过缺失帧。
        //
        // 两种情况会跳过：
        // 1. 最老帧已经等太久了
        // 2. jitter buffer 已经堆太多帧了
        const bool should_skip_missing =
                (max_wait_ns > 0 && oldest_wait_ns >= max_wait_ns) ||
                udp_jitter_buffer_.size() >= std::max<std::size_t>(effective_max_frames, 1);

        // 如果还不应该跳过缺失帧，就继续等
        if (!should_skip_missing) {
            return false;
        }

        // 如果最老帧序号大于期望序号，说明中间确实跳过了一些帧
        if (oldest->first > next_jitter_buffer_sequence_) {
            udp_receive_stats_.jitter_buffer_skipped_frames += oldest->first - next_jitter_buffer_sequence_;
        }

        // 把下一帧期望序号更新为当前最老帧。
        // 意思是：缺的帧不等了，先播放/解码已有的最老帧。
        next_jitter_buffer_sequence_ = oldest->first;
        expected = oldest;
    }

    // 计算 expected 这帧已经在 jitter buffer 里等待了多久
    const std::uint64_t wait_ns = now_ns > expected->second.buffered_timestamp_ns
            ? now_ns - expected->second.buffered_timestamp_ns
            : 0;

    // 判断是否已经达到目标延迟
    const bool reached_target_delay = target_delay_ns == 0 || wait_ns >= target_delay_ns;
    // 判断是否达到缓存深度限制
    const bool reached_depth_limit =
            udp_jitter_buffer_.size() >= std::max<std::size_t>(effective_max_frames, 1);
    // 判断是否达到最大等待时间
    const bool reached_max_wait = max_wait_ns > 0 && wait_ns >= max_wait_ns;

    // 如果三个条件都没满足：
    // 1. 没达到目标延迟
    // 2. 没达到缓存深度限制
    // 3. 没达到最大等待时间
    //
    // 那就暂时不输出这一帧，继续等。
    if (!reached_target_delay && !reached_depth_limit && !reached_max_wait) {
        return false;
    }

    // 到这里说明 expected 这帧可以输出了。
    //
    // std::move 表示把 buffer 中的 frame 移动到输出参数 frame，
    // 避免拷贝大块 H.264 payload。
    *frame = std::move(expected->second.frame);

    // 记录这一帧在 jitter buffer 里等待了多久
    RecordJitterBufferWait(wait_ns);

    // jitter buffer 输出帧数 +1
    ++udp_receive_stats_.jitter_buffer_emitted_frames;

    // 从 jitter buffer 中删除这帧
    udp_jitter_buffer_.erase(expected);

    // 下一次期望输出的帧序号 +1
    ++next_jitter_buffer_sequence_;

    // 如果 jitter buffer 已经空了
    if (udp_jitter_buffer_.empty()) {
        has_next_jitter_buffer_sequence_ = false;
    }

    // 更新 jitter buffer 当前深度和最大深度统计
    UpdateJitterBufferDepthStats();
    return true;
}

// 当某一帧的 UDP 分片全部收齐后，把 UdpFrameAssembly 转成完整的 ReceivedFrame。
//
// UdpFrameAssembly 是“拼帧过程中的临时状态”。
// ReceivedFrame 是“已经拼好的完整编码帧”，可以交给解码器。
bool StreamClient::FinalizeCompletedUdpFrame(
        std::uint64_t frame_sequence,
        UdpFrameAssembly *assembly,
        std::uint64_t now_ns,
        bool defer_ready_pop,
        ReceivedFrame *frame) {

    // 做基本保护：
    //
    // 1. assembly 不能为空。
    // 2. frame 不能为空。
    // 3. 已收到分片数必须等于总分片数。
    //
    // 如果还有分片没收齐，就不能 finalize。
    if (assembly == nullptr || frame == nullptr || assembly->received_fragment_count != assembly->received_fragments.size()) {
        return false;
    }

    ReceivedFrame completed_frame;
    completed_frame.header = assembly->header;
    completed_frame.metadata = assembly->metadata;
    completed_frame.sender_metadata_available =
            config_.expect_metadata && network_internal::HasSenderLatencyMetadata(completed_frame.metadata);
    completed_frame.receive_timestamp_ns = now_ns;
    completed_frame.payload.swap(assembly->payload);
    completed_frame.receive_timestamp_ns += InjectedUdpJitterDelayNs(completed_frame);
    ++udp_receive_stats_.completed_frames;

    // 完成一帧后，更新一些统计。
    //
    // 例如：
    // 1. 是否乱序
    // 2. 网络延迟
    // 3. jitter 平均值、最大值
    // 4. 自适应 jitter buffer 策略
    OnCompletedUdpFrame(completed_frame, now_ns);

    // 记住这个 frame_sequence 已经完成过。
    //
    // 作用：
    // 如果之后又收到这一帧的旧分片或者重传分片，
    // 就可以直接丢弃，避免重复处理。
    RememberCompletedUdpFrameSequence(frame_sequence);

    // 清理测试丢包记录。
    //
    // 因为这一帧已经完成了，之前为了模拟丢包记录的状态就没用了
    udp_test_dropped_fragments_.erase(frame_sequence);

    // 从 udp_assemblies_ 中删除这一帧的重组状态
    udp_assemblies_.erase(frame_sequence);

    // 如果没有开启 UDP jitter buffer
    if (!config_.udp_jitter_buffer_enabled) {
        *frame = std::move(completed_frame);
        return true;
    }

    // 如果开启了 jitter buffer，
    // 就先把完整帧放进 jitter buffer，而不是马上交给解码器。
    BufferCompletedUdpFrame(std::move(completed_frame));

    // 如果 defer_ready_pop 为 true，
    // 表示当前阶段只是先处理 datagram，暂时不要从 jitter buffer 里拿帧。
    if (defer_ready_pop) {
        return false;
    }

    // 如果允许立即弹出帧，就先检查是否需要发送 NACK。
    //
    // 这里的逻辑是：
    // jitter buffer 可能在等某些缺失帧；
    // 如果发现前面的帧缺分片，就请求服务端重传。
    MaybeSendUdpNackRequests(now_ns);

    // 尝试从 jitter buffer 中取出一帧可以输出的完整帧
    return TryPopReadyUdpFrame(frame, now_ns);
}

// 获取当前 jitter buffer 的目标等待时间，单位纳秒
std::uint64_t StreamClient::CurrentJitterBufferTargetDelayNs() {
    // 读取当前 jitter buffer 策略
    const std::string &mode = config_.udp_jitter_buffer_strategy;

    // 如果模式是 auto / off / low / smooth，
    // 这些模式走 adaptive_jitter_ 的统一逻辑。
    if (mode == "auto" || mode == "off" || mode == "low" || mode == "smooth") {

        // 从 adaptive_jitter_ 里获取当前目标延迟。
        //
        // adaptive_jitter_ 会根据网络质量动态调整。
        // 网络稳定时，目标延迟小；
        // 网络抖动大时，目标延迟变大。
        const std::uint64_t target_ns = adaptive_jitter_.TargetDelayNs();
        udp_receive_stats_.jitter_buffer_target_delay_ms =
                static_cast<double>(target_ns) / 1000000.0;
        return target_ns;
    }

    // 如果不是上面的模式，就先使用配置里的固定目标延迟。
    //
    // std::max(..., 0) 是为了避免配置成负数。
    double target_delay_ms = static_cast<double>(std::max(config_.udp_jitter_buffer_target_delay_ms, 0));

    // 如果模式是 adaptive，并且已经有 jitter 样本，
    // 就根据实际 jitter 再增加一点目标延迟。
    if (mode == "adaptive" && udp_receive_stats_.jitter_samples > 0) {
        // 计算 adaptive 模式下允许的最大延迟。
        //
        // 它取：
        // udp_jitter_buffer_adaptive_max_delay_ms
        // 和
        // udp_jitter_buffer_target_delay_ms
        // 中较大的那个，避免最大值比基础目标值还小。
        const double adaptive_max_delay_ms = static_cast<double>(
                std::max(config_.udp_jitter_buffer_adaptive_max_delay_ms, config_.udp_jitter_buffer_target_delay_ms));

        // 根据 jitter 平均值动态增加目标延迟。
        //
        // target_delay_ms + jitter_avg_ms * 2.0
        //
        // 直观理解：
        // 如果网络抖动平均 5ms，
        // 那目标等待时间可以在基础值上多加 10ms。
        //
        // 同时不能超过 adaptive_max_delay_ms。
        target_delay_ms = std::min(adaptive_max_delay_ms, target_delay_ms + udp_receive_stats_.jitter_avg_ms * 2.0);
    }

    udp_receive_stats_.jitter_buffer_target_delay_ms = target_delay_ms;
    return static_cast<std::uint64_t>(target_delay_ms * 1000000.0);
}


// 根据测试配置，计算要人为注入多少 UDP jitter delay。
//
// 注意：这个函数不是正常业务必需逻辑。
// 它主要用于测试 jitter buffer 在不同网络抖动下的表现。
std::uint64_t StreamClient::InjectedUdpJitterDelayNs(const ReceivedFrame &frame) const {

    // 如果测试抖动模式是 none，或者抖动幅度 <= 0，
    // 表示不注入任何抖动。
    if (config_.udp_test_jitter_pattern == "none" || config_.udp_test_jitter_amplitude_ms <= 0) {
        return 0;
    }

    // 读取抖动周期。
    //
    // std::max(..., 2) 保证周期至少是 2。
    // 因为有些模式需要根据周期计算相位。
    const int period = std::max(config_.udp_test_jitter_period, 2);
    const std::uint64_t sequence = frame.metadata.sequence;
    double delay_ms = 0.0;

    // saw 表示锯齿形抖动。
    //
    // 例如 amplitude=40ms，period=5：
    //
    // sequence phase:
    // 0 -> 0ms
    // 1 -> 10ms
    // 2 -> 20ms
    // 3 -> 30ms
    // 4 -> 40ms
    // 然后下一轮再从 0ms 开始。
    if (config_.udp_test_jitter_pattern == "saw") {
        const int phase = static_cast<int>(sequence % static_cast<std::uint64_t>(period));
        delay_ms = static_cast<double>(config_.udp_test_jitter_amplitude_ms) *
                static_cast<double>(phase) /
                static_cast<double>(period - 1);

    // burst 表示突发抖动。
    //
    // 周期前半段加延迟，后半段不加延迟。
    } else if (config_.udp_test_jitter_pattern == "burst") {
        const int phase = static_cast<int>(sequence % static_cast<std::uint64_t>(period));
        delay_ms = phase < (period / 2)
                ? static_cast<double>(config_.udp_test_jitter_amplitude_ms)
                : 0.0;

    // alternate 表示交替抖动。
    //
    // 偶数帧不加延迟，奇数帧加延迟。
    } else if (config_.udp_test_jitter_pattern == "alternate") {
        delay_ms = (sequence % 2ULL) == 0ULL
                ? 0.0
                : static_cast<double>(config_.udp_test_jitter_amplitude_ms);
    }

    return static_cast<std::uint64_t>(delay_ms * 1000000.0);
}

// 根据测试配置，判断当前 UDP 数据分片是否要被人为丢弃。
//
// 这个函数也是测试用的。
// 它可以模拟真实网络中的 UDP 丢包，用来测试 NACK 和 FEC 是否有效。
bool StreamClient::ShouldInjectUdpLoss(const protocol::UdpFrameFragmentHeader &fragment_header) {
    // 如果丢包模式是 none，说明不模拟丢包。
    //
    // 如果 fragment_count == 0，说明分片信息不合法，不在这里处理。
    //
    // 如果当前包不是普通数据分片，也不丢。
    // 也就是说：这里只对 kData 分片模拟丢包，不对 FEC 分片模拟丢包。
    if (config_.udp_test_loss_pattern == "none" ||
        fragment_header.fragment_count == 0 ||
        fragment_header.fragment_role != static_cast<std::uint16_t>(protocol::UdpFragmentRole::kData)) {
        return false;
    }

    bool should_drop = false;

    // single 模式：按周期丢某一帧的第 0 个分片。
    //
    // 例如 period = 30：
    // 第 0、30、60、90 帧的 fragment 0 会被丢。
    if (config_.udp_test_loss_pattern == "single") {
        const std::uint64_t period = static_cast<std::uint64_t>(std::max(config_.udp_test_loss_period, 1));
        should_drop = (fragment_header.frame_sequence % period) == 0ULL && fragment_header.fragment_index == 0;

    // burst 模式：按周期连续丢多个分片。
    //
    // 例如 period = 30，drop_count = 3：
    // 第 0、30、60 帧中，fragment 0、1、2 都会被丢。
    } else if (config_.udp_test_loss_pattern == "burst") {
        const std::uint64_t period = static_cast<std::uint64_t>(std::max(config_.udp_test_loss_period, 1));
        const std::size_t drop_count = std::max(config_.udp_test_loss_count, 1);
        should_drop = (fragment_header.frame_sequence % period) == 0ULL &&
                fragment_header.fragment_index < std::min<std::size_t>(drop_count, fragment_header.fragment_count);

    // alternate 模式：隔一帧丢一次第 0 个分片。
    //
    // 奇数帧的 fragment 0 被丢。
    } else if (config_.udp_test_loss_pattern == "alternate") {
        should_drop = (fragment_header.frame_sequence % 2ULL) == 1ULL && fragment_header.fragment_index == 0;
    } else {
        return false;
    }

    // 如果根据上面的规则判断不需要丢，就返回 false
    if (!should_drop) {
        return false;
    }

    // 记录当前 frame_sequence 已经被测试丢过哪些分片。
    //
    // 为什么要记录？
    // 因为服务端可能因为 NACK 重传同一个分片。
    // 如果我们每次都把这个分片继续人为丢掉，
    // 那 NACK 永远无法恢复。
    //
    // 所以这里的设计是：
    // 同一个 frame_sequence 的同一个 fragment_index，只人为丢一次。
    std::vector<bool> &dropped_fragments = udp_test_dropped_fragments_[fragment_header.frame_sequence];

    // 如果这个帧序号还没有丢包记录，就初始化记录数组
    if (dropped_fragments.empty()) {
        dropped_fragments.assign(fragment_header.fragment_count, false);

    // 如果已有记录，但 fragment_count 对不上，就重新初始化
    } else if (dropped_fragments.size() != fragment_header.fragment_count) {
        dropped_fragments.assign(fragment_header.fragment_count, false);
    }

    // 如果这个 fragment_index 之前已经被人为丢过一次
    if (dropped_fragments[fragment_header.fragment_index]) {
        // 这次就不再丢
        return false;
    }

    // 标记这个 fragment_index 已经被人为丢过
    dropped_fragments[fragment_header.fragment_index] = true;

    // 注入丢包 datagram 统计 +1
    ++udp_receive_stats_.injected_loss_datagrams;

    // 注入丢包分片统计 +1
    ++udp_receive_stats_.injected_loss_fragments;
    return true;
}

// 尝试用 UDP FEC 恢复缺失的数据分片
bool StreamClient::MaybeRecoverWithUdpFec(UdpFrameAssembly *assembly) {

    if (!config_.udp_fec_enabled ||
        // 配置没有开启 UDP FEC。
        assembly == nullptr ||
        // assembly 为空，没有可恢复对象。
        !assembly->has_fec_payload ||
        // 没收到 FEC 校验分片。
        assembly->fec_payload_timestamp_ns == 0 ||
        // FEC 校验分片时间戳无效。
        assembly->received_fragments.empty() ||
        // 没有分片接收状态。
        assembly->received_fragment_count >= assembly->received_fragments.size()) {
        // 已经收齐了，或者收到数量异常，也不需要恢复。
        return false;
    }

    // missing_index 用来记录缺失的是第几个分片。
    //
    // 先初始化成 size，表示“暂时还不知道缺哪个”。
    std::size_t missing_index = assembly->received_fragments.size();
    std::size_t missing_count = 0;

    // 遍历 received_fragments，找出没收到的分片
    for (std::size_t index = 0; index < assembly->received_fragments.size(); ++index) {
        // 如果这个 index 的分片没收到
        if (!assembly->received_fragments[index]) {
            // 记录缺失分片的 index
            missing_index = index;
            ++missing_count;

            // 如果发现缺失超过 1 个分片
            if (missing_count > 1) {
                // XOR FEC 无法恢复两个及以上缺失分片
                return false;
            }
        }
    }

    // 如果缺失数量不是 1，也不能恢复。
    //
    // missing_count == 0：说明没缺分片，不需要恢复。
    // missing_count > 1：上面已经返回了
    if (missing_count != 1) {
        return false;
    }

    // 走到这里说明：
    // 1. FEC 已开启
    // 2. 已经收到 FEC 校验分片
    // 3. 当前刚好只缺一个数据分片

    // missing_offset 表示缺失分片在完整 payload 中的起始位置。
    std::uint32_t missing_offset = 0;

    // 如果缺失的不是第 0 个分片
    if (missing_index > 0) {
        // 找到前一个分片的大小
        const std::uint32_t previous_size = assembly->fragment_payload_sizes[missing_index - 1];

        // 缺失分片的 offset = 前一个分片 offset + 前一个分片 size。
        missing_offset = assembly->fragment_offsets[missing_index - 1] + previous_size;
    }

    // missing_size 表示缺失分片的大小
    std::uint32_t missing_size = 0;

    // 如果缺失的不是最后一个分片
    if (missing_index + 1 < assembly->received_fragments.size()) {
        // 用下一个分片的 offset 推出当前缺失分片的 size。
        //
        // 举例：
        // fragment 1 缺失
        // fragment 2 offset = 2800
        // missing_offset = 1400
        //
        // missing_size = 2800 - 1400 = 1400
        const std::uint32_t next_offset = assembly->fragment_offsets[missing_index + 1];

        // 如果下一个分片 offset 比当前 missing_offset 还小，
        // 说明数据异常，不能恢复。
        if (next_offset < missing_offset) {
            return false;
        }

        // 当前缺失分片大小 = 下一个分片起点 - 当前缺失分片起点。
        missing_size = next_offset - missing_offset;

    // 如果缺失的是最后一个分片。
    } else if (assembly->payload.size() >= missing_offset) {
        // 最后一个分片大小 =
        // 完整 payload 总大小 - 最后一个分片 offset。
        //
        // 举例：
        // payload 总大小 5000
        // fragment 3 缺失
        // missing_offset = 4200
        // missing_size = 5000 - 4200 = 800
        missing_size = static_cast<std::uint32_t>(assembly->payload.size() - missing_offset);
    }

    // 检查 missing_size 是否合理
    if (missing_size == 0 || missing_size > assembly->fec_payload.size() ||
        static_cast<std::size_t>(missing_offset) + missing_size > assembly->payload.size()) {
        // 三种情况不能恢复：
        // 1. 缺失分片大小为 0
        // 2. 缺失分片比 FEC payload 还大
        // 3. 写回 payload 时会越界
        return false;
    }

    // recovered 先复制一份 FEC 校验数据。
    //
    // 理论上：
    // recovered = parity
    std::vector<std::uint8_t> recovered = assembly->fec_payload;

    // 遍历所有分片。
    for (std::size_t index = 0; index < assembly->received_fragments.size(); ++index) {
        // 如果这个分片没收到，也就是缺失的那个分片
        if (!assembly->received_fragments[index]) {
            continue;
        }

        // 取出当前已收到分片在完整 payload 中的 offset
        const std::uint32_t fragment_offset = assembly->fragment_offsets[index];

        // 取出当前已收到分片的大小
        const std::uint32_t fragment_size = assembly->fragment_payload_sizes[index];

        // 检查这个已收到分片是否越界
        if (static_cast<std::size_t>(fragment_offset) + fragment_size > assembly->payload.size()) {
            return false;
        }

        // 用 XOR 把当前已收到分片从 parity 中“抵消掉”。
        //
        // recovered 初始是：
        // data0 ^ data1 ^ data2 ^ data3
        //
        // 如果缺的是 data2，循环中会执行：
        // recovered ^= data0
        // recovered ^= data1
        // recovered ^= data3
        //
        // 最后剩下：
        // recovered = data2
        XorInto(&recovered, assembly->payload.data() + fragment_offset, fragment_size);
    }

    // 把恢复出来的分片写回完整 payload 的缺失位置。
    std::memcpy(
            assembly->payload.data() + missing_offset,
            recovered.data(),
            missing_size);
    // 标记缺失分片现在已经恢复成功。
    assembly->received_fragments[missing_index] = true;

    // 保存恢复分片的 offset。
    assembly->fragment_offsets[missing_index] = missing_offset;

    // 保存恢复分片的 size。
    assembly->fragment_payload_sizes[missing_index] = missing_size;

    // 已收到分片数 +1。
    ++assembly->received_fragment_count;

    // FEC 恢复分片统计 +1。
    ++udp_receive_stats_.fec_recovered_fragments;

    if (assembly->received_fragment_count == assembly->received_fragments.size()) {
        ++udp_receive_stats_.fec_recovered_frames;
    }
    return true;
}

// 判断某个 UDP frame_sequence 是否最近已经完成过。
//
// 如果已经完成过，说明这帧已经交给解码器或 jitter buffer 了。
// 后面再收到同一帧的旧分片，就可以直接丢掉。
bool StreamClient::IsRecentlyCompletedUdpFrameSequence(std::uint64_t sequence) const {
    return recently_completed_udp_sequences_.find(sequence) != recently_completed_udp_sequences_.end();
}

// 记住某个 UDP frame_sequence 最近已经完成过。
//
// 为什么要记？
// 因为 UDP 可能乱序，也可能因为 NACK 导致服务端重传旧分片。
// 如果旧分片迟到了，客户端不能重新创建 assembly 再拼一次。
void StreamClient::RememberCompletedUdpFrameSequence(std::uint64_t sequence) {
    const std::pair<std::unordered_set<std::uint64_t>::iterator, bool> inserted =
            recently_completed_udp_sequences_.insert(sequence);

    // 如果没有插入成功，说明之前已经记录过了
    if (!inserted.second) {
        return;
    }

    // 把 sequence 也放入 order 队列，用于维护先后顺序
    recently_completed_udp_sequence_order_.push_back(sequence);

    // 如果记录的已完成帧太多，就删除最旧的。
    //
    // kRecentCompletedUdpSequenceWindow 是窗口大小。
    // 例如最多记录最近 1024 帧。
    while (recently_completed_udp_sequence_order_.size() > kRecentCompletedUdpSequenceWindow) {
        // 取出最旧的 sequence
        const std::uint64_t oldest_sequence = recently_completed_udp_sequence_order_.front();

        // 从顺序队列里弹出
        recently_completed_udp_sequence_order_.pop_front();

        // 从 set 里删除
        recently_completed_udp_sequences_.erase(oldest_sequence);
    }
}

// 检查当前所有未完成的 UDP 帧，必要时发送 NACK 请求
void StreamClient::MaybeSendUdpNackRequests(std::uint64_t now_ns) {
    // 创建统计快照保护对象。
    // 函数退出时会发布 UDP 接收统计快照。
    UdpStatsSnapshotGuard snapshot_guard(this);

    // 如果没有开启 UDP NACK，直接返回。
    //
    // 或者如果每个 NACK 包允许请求的分片数为 0，也没法发。
    if (!config_.udp_nack_enabled || config_.udp_nack_max_requests_per_packet == 0) {
        return;
    }

    // 计算第一次发送 NACK 前需要等待多久
    const std::uint64_t nack_delay_ns =
            static_cast<std::uint64_t>(std::max(config_.udp_nack_request_delay_ms, 0)) * 1000ULL * 1000ULL;

    // 计算两次 NACK 之间的最小间隔
    const std::uint64_t nack_retry_ns =
            static_cast<std::uint64_t>(std::max(config_.udp_nack_retry_interval_ms, 0)) * 1000ULL * 1000ULL;

    // 遍历所有正在重组但还没完成的 UDP 帧
    for (std::map<std::uint64_t, UdpFrameAssembly>::iterator it = udp_assemblies_.begin();
         it != udp_assemblies_.end();
         ++it) {
        // 取出当前帧的重组状态
        UdpFrameAssembly &assembly = it->second;

        // 一组过滤条件。
        // 满足任意一个，就跳过这一帧，不发 NACK。
        if (assembly.received_fragments.empty() ||
            // 没有初始化分片状态，跳过。
            assembly.received_fragment_count >= assembly.received_fragments.size() ||
            // 已经收齐了，不需要 NACK。
            assembly.first_seen_timestamp_ns == 0 ||
            // 第一次看到这一帧的时间无效。
            now_ns < assembly.first_seen_timestamp_ns ||
            // 当前时间比首次看到时间还早，时间异常。
            now_ns - assembly.first_seen_timestamp_ns < nack_delay_ns ||
            // 还没等到 nack_delay，不急着发。
            assembly.nack_attempts >= config_.udp_nack_max_retries) {
            // NACK 尝试次数已经达到上限。
            continue;
        }

        // 如果之前已经发过 NACK，
        // 并且距离上次 NACK 的时间还没超过 retry 间隔，
        // 就不要重复发。
        if (assembly.last_nack_timestamp_ns != 0 &&
            now_ns >= assembly.last_nack_timestamp_ns &&
            now_ns - assembly.last_nack_timestamp_ns < nack_retry_ns) {
            continue;
        }

        // 用来保存当前帧缺失的 fragment index
        std::vector<std::uint16_t> missing_fragments;
        missing_fragments.reserve(assembly.received_fragments.size() - assembly.received_fragment_count);

        // 遍历这一帧的所有分片状态
        for (std::size_t fragment_index = 0; fragment_index < assembly.received_fragments.size(); ++fragment_index) {
            // 如果这个分片还没收到
            if (!assembly.received_fragments[fragment_index]) {
                // 把它加入缺失列表
                missing_fragments.push_back(static_cast<std::uint16_t>(fragment_index));
            }
        }

        // 如果没有缺失分片，就不发 NACK
        if (missing_fragments.empty()) {
            continue;
        }

        // 发送 NACK 请求。
        //
        // 告诉服务端：
        // assembly.metadata.sequence 这帧，
        // missing_fragments 这些分片没收到。
        if (SendUdpNack(assembly.metadata.sequence, missing_fragments)) {
            assembly.last_nack_timestamp_ns = now_ns;
            ++assembly.nack_attempts;
        }
    }
}

// 构造并发送 UDP NACK 请求包。
//
// 参数：
// frame_sequence：哪一帧缺分片。
// missing_fragments：这一帧缺哪些 fragment index。
bool StreamClient::SendUdpNack(std::uint64_t frame_sequence, const std::vector<std::uint16_t> &missing_fragments) {
    if (socket_fd_ < 0 || missing_fragments.empty()) {
        return false;
    }

    // 每个 NACK 包最多携带多少个缺失分片请求。
    //
    // 例如 missing_fragments 有 20 个，
    // max_requests = 8，
    // 那么会拆成 3 个 NACK 包：
    // 第 1 个请求 8 个
    // 第 2 个请求 8 个
    // 第 3 个请求 4 个
    const std::size_t max_requests = std::max<std::size_t>(1, config_.udp_nack_max_requests_per_packet);
    // 按 max_requests 分批发送 NACK。
    for (std::size_t offset = 0; offset < missing_fragments.size(); offset += max_requests) {
        // 本次 NACK 包要请求多少个分片。
        const std::size_t request_count = std::min(max_requests, missing_fragments.size() - offset);

        // 构造项目自定义协议头。
        protocol::MessageHeader header{};

        // 填写 magic/head_id，表示这是本项目协议包。
        protocol::FillMessageMagic(header.head_id);

        // 消息类型设置为 kUdpNack。
        // 服务端收到后就知道：这是客户端请求 UDP 分片重传。
        header.message_type = static_cast<std::uint16_t>(protocol::MessageType::kUdpNack);

        // payload_length 表示 MessageHeader 后面的长度。
        //
        // NACK payload 结构：
        // [UdpNackHeader][UdpNackItem][UdpNackItem]...
        header.payload_length = static_cast<std::uint32_t>(
                sizeof(protocol::UdpNackHeader) + request_count * sizeof(protocol::UdpNackItem));

        // 构造 NACK 头。
        protocol::UdpNackHeader nack_header{};

        // 记录请求发送时间，单位纳秒。
        // 服务端可以用它做诊断统计。
        nack_header.request_timestamp_ns = MonotonicNowNs();

        // 记录这个 NACK 包里有多少个 UdpNackItem。
        nack_header.request_count = static_cast<std::uint16_t>(request_count);

        // 准备完整 UDP NACK 包的字节 buffer。
        //
        // 完整结构：
        // [MessageHeader][UdpNackHeader][UdpNackItem...]
        std::vector<char> payload(sizeof(header) + header.payload_length);

        // 拷贝 MessageHeader 到 payload 开头。
        std::memcpy(payload.data(), &header, sizeof(header));

        // 拷贝 UdpNackHeader 到 MessageHeader 后面。
        std::memcpy(payload.data() + sizeof(header), &nack_header, sizeof(nack_header));

        // 逐个填写 UdpNackItem。
        for (std::size_t index = 0; index < request_count; ++index) {
            // 创建一个 NACK item。
            protocol::UdpNackItem item{};

            // 告诉服务端：缺的是哪一帧。
            item.frame_sequence = frame_sequence;

            // 告诉服务端：缺的是这一帧的哪个分片。
            item.fragment_index = missing_fragments[offset + index];

            // 把 item 拷贝进 payload。
            std::memcpy(
                    payload.data() + sizeof(header) + sizeof(nack_header) + index * sizeof(item),
                    &item,
                    sizeof(item));
        }

        // 发送时加锁。
        //
        // 因为客户端可能还有 keepalive 线程也会用同一个 socket send。
        // 加锁可以避免两个线程同时写 socket 造成数据混乱。
        std::lock_guard<std::mutex> lock(send_mutex_);

        // 通过 UDP socket 发送 NACK 包。
        //
        // MSG_NOSIGNAL 表示如果 socket 出错，不要触发 SIGPIPE。
        const ssize_t sent = send(socket_fd_, payload.data(), payload.size(), MSG_NOSIGNAL);

        // 如果实际发送字节数不等于 payload 大小，认为失败。
        if (sent != static_cast<ssize_t>(payload.size())) {
            return false;
        }

        // NACK 请求包数量 +1。
        ++udp_receive_stats_.nack_requests_sent;

        // NACK 请求的分片数量累计。
        udp_receive_stats_.nack_fragments_requested += request_count;
    }

    // 所有 NACK 包都发送成功。
    return true;
}

// 重置 UDP 相关状态。
//
// 什么时候会调用？
// 通常在 Connect() 连接新服务端时，或者重新初始化 UDP 接收逻辑时调用。
// 目的是把上一轮连接留下来的统计、缓存、重组状态都清空。
void StreamClient::ResetUdpState() {
    // 把 UDP 接收统计重置成默认值。
    //
    // UdpReceiveStats() 会创建一个全新的统计对象，
    // 所有计数基本都会回到 0。
    udp_receive_stats_ = UdpReceiveStats();

    // 初始化 jitter buffer 目标延迟统计。
    //
    // 这里使用配置中的 udp_jitter_buffer_target_delay_ms。
    // std::max(..., 0) 防止配置成负数。
    udp_receive_stats_.jitter_buffer_target_delay_ms =
            static_cast<double>(std::max(config_.udp_jitter_buffer_target_delay_ms, 0));

    // 最近完成的帧序号重置为 0。
    last_completed_frame_sequence_ = 0;

    // 标记：当前还没有“上一帧已完成序号”。
    has_last_completed_frame_sequence_ = false;

    // jitter buffer 下一帧期望输出序号重置为 0。
    next_jitter_buffer_sequence_ = 0;

    // 标记：当前还没有有效的 next_jitter_buffer_sequence_。
    has_next_jitter_buffer_sequence_ = false;

    // 清空测试丢包记录。
    //
    // 这个 map 用来记录某帧某分片是否已经被“人为丢过一次”。
    udp_test_dropped_fragments_.clear();

    // 清空最近完成帧的顺序队列。
    recently_completed_udp_sequence_order_.clear();

    // 清空最近完成帧的 set。
    recently_completed_udp_sequences_.clear();

    // 上一次网络延迟统计重置。
    previous_network_latency_ms_ = 0.0;

    // 标记：目前还没有上一帧网络延迟样本。
    has_previous_network_latency_ = false;

    // 重置自适应 jitter buffer 状态。
    adaptive_jitter_.Reset();

    // 发布一次 UDP 统计快照。
    //
    // 这样 UI/HUD 上看到的是重置后的统计。
    PublishUdpReceiveStatsSnapshot();
}

// 重置 RTP 相关状态。
//
// 虽然这个文件叫 StreamClientUdp.cpp，
// 但 RTP 本质上也是 UDP socket 接收，所以部分状态函数放在这里。
void StreamClient::ResetRtpState() {
    // 清空 RTP 当前帧正在组装的 payload。
    rtp_frame_assembly_.payload.clear();

    // RTP timestamp 重置。
    rtp_frame_assembly_.timestamp = 0;

    // RTP SSRC 重置。
    // SSRC 用来标识一条 RTP 流。
    rtp_frame_assembly_.ssrc = 0;

    // 下一期望 RTP sequence number 重置。
    rtp_frame_assembly_.next_sequence_number = 0;

    // 采集时间戳重置。
    rtp_frame_assembly_.capture_timestamp_ns = 0;

    // 服务端发送时间戳重置。
    rtp_frame_assembly_.transport_send_timestamp_ns = 0;

    // 标记当前没有活跃 RTP 帧正在组装。
    rtp_frame_assembly_.active = false;

    // 标记还没有 RTP sequence number。
    rtp_frame_assembly_.has_sequence_number = false;

    // 标记发送端 metadata 不可用。
    rtp_frame_assembly_.sender_metadata_available = false;

    // 标记发送端 metadata 没有异常。
    rtp_frame_assembly_.sender_metadata_invalid = false;

    // 标记当前帧没有损坏。
    rtp_frame_assembly_.frame_damaged = false;

    // 标记当前没有 FU-A 分片正在组装。
    rtp_frame_assembly_.fu_in_progress = false;
}

// 清理已经超时的 UDP 帧重组状态。
//
// UDP 一帧可能被拆成多个 fragment。
// 如果某一帧迟迟收不齐，就不能一直留在内存里。
// 否则 udp_assemblies_ 会越积越多。
void StreamClient::PruneExpiredUdpAssemblies(std::uint64_t now_ns) {
    // 创建统计快照保护对象。
    UdpStatsSnapshotGuard snapshot_guard(this);

    // 遍历所有正在重组的 UDP 帧。
    //
    // 注意这里没有 ++it 写在 for 第三段，
    // 因为循环中可能 erase 当前 it。
    for (std::map<std::uint64_t, UdpFrameAssembly>::iterator it = udp_assemblies_.begin();
         it != udp_assemblies_.end();) {

        // 取出当前帧的 assembly。
        const UdpFrameAssembly &assembly = it->second;

        // 判断是否超时：
        //
        // 1. last_fragment_timestamp_ns != 0
        //    说明这一帧确实收到过至少一个分片。
        //
        // 2. now_ns > last_fragment_timestamp_ns
        //    当前时间比最后收到分片时间晚。
        //
        // 3. now_ns - last_fragment_timestamp_ns > kUdpAssemblyTimeoutNs
        //    距离最后收到分片已经超过允许时间。
        if (assembly.last_fragment_timestamp_ns != 0 &&
            now_ns > assembly.last_fragment_timestamp_ns &&
            now_ns - assembly.last_fragment_timestamp_ns > kUdpAssemblyTimeoutNs) {
            // 超时帧数量 +1。
            ++udp_receive_stats_.timed_out_frames;

            // 累计这一帧没收到的分片数量。
            //
            // 总分片数 - 已收到分片数 = 缺失分片数。
            udp_receive_stats_.timed_out_fragments += static_cast<std::uint64_t>(
                    assembly.received_fragments.size() - assembly.received_fragment_count);

            // 清理测试丢包记录。
            udp_test_dropped_fragments_.erase(it->first);

            // 从 map 中删除这个超时 assembly。
            //
            // erase 返回删除后下一个迭代器，所以赋值给 it。
            it = udp_assemblies_.erase(it);

            // 继续下一轮，不执行下面的 ++it。
            continue;
        }

        // 如果没有超时，就检查下一个 assembly。
        ++it;
    }
}

// 把已经拼完整的 UDP 帧放入 jitter buffer。
//
// 注意：只有开启 config_.udp_jitter_buffer_enabled 时，
// FinalizeCompletedUdpFrame 才会调用它。
void StreamClient::BufferCompletedUdpFrame(ReceivedFrame &&frame) {
    // 取出这一帧的序号。
    const std::uint64_t sequence = frame.metadata.sequence;

    // 如果 jitter buffer 里已经有同一 sequence 的帧。
    if (udp_jitter_buffer_.find(sequence) != udp_jitter_buffer_.end()) {
        // 说明重复了，丢弃。
        ++udp_receive_stats_.jitter_buffer_dropped_frames;

        // 直接返回。
        return;
    }

    // 创建一个 BufferedUdpFrame。
    BufferedUdpFrame buffered_frame;

    // 记录进入 jitter buffer 的时间。
    //
    // 这里用 frame.receive_timestamp_ns。
    buffered_frame.buffered_timestamp_ns = frame.receive_timestamp_ns;

    // 把完整帧移动到 buffered_frame。
    buffered_frame.frame = std::move(frame);

    // 以 sequence 为 key，放进 jitter buffer。
    //
    // udp_jitter_buffer_ 是 map，所以天然按 sequence 排序。
    udp_jitter_buffer_.emplace(sequence, std::move(buffered_frame));

    // 更新 jitter buffer 当前深度统计。
    UpdateJitterBufferDepthStats();
}

// 当某一帧 UDP 完整重组完成后，更新统计信息。
//
// 主要做三件事：
// 1. 检测帧是否乱序。
// 2. 计算网络延迟。
// 3. 计算网络抖动 jitter，并喂给 adaptive_jitter_。
void StreamClient::OnCompletedUdpFrame(const ReceivedFrame &frame, std::uint64_t now_ns) {
    // 取出当前帧序号。
    const std::uint64_t sequence = frame.metadata.sequence;

    // 如果之前已经完成过一帧，并且当前帧不是上一帧 + 1，
    // 就认为出现了重排序/跳帧。
    //
    // 举例：
    // 上一帧是 100，当前帧是 102。
    // 中间少了 101，或者 101 还没到。
    if (has_last_completed_frame_sequence_ && sequence != last_completed_frame_sequence_ + 1) {
        // 乱序/不连续帧计数 +1。
        ++udp_receive_stats_.reordered_frames;
    }

    // 更新最后完成帧序号。
    last_completed_frame_sequence_ = sequence;

    // 标记已经有最后完成帧序号。
    has_last_completed_frame_sequence_ = true;

    // 如果没有服务端发送时间戳，或者接收时间比发送时间还早，
    // 就无法计算网络延迟。
    if (frame.metadata.transport_send_timestamp_ns == 0 || frame.receive_timestamp_ns < frame.metadata.transport_send_timestamp_ns) {
        return;
    }

    // 计算网络延迟：
    //
    // network_latency = 客户端收到完整帧时间 - 服务端发送时间
    //
    // 单位从纳秒转成毫秒。
    const double network_latency_ms = static_cast<double>(
            frame.receive_timestamp_ns - frame.metadata.transport_send_timestamp_ns) / 1000000.0;

    // 如果之前已经有上一帧网络延迟样本。
    if (has_previous_network_latency_) {
        // jitter 的简单计算方式：
        //
        // 当前帧网络延迟 和 上一帧网络延迟 的差值绝对值。
        //
        // 举例：
        // 上一帧网络延迟 20ms
        // 当前帧网络延迟 35ms
        // jitter = |35 - 20| = 15ms
        const double jitter_ms = std::abs(network_latency_ms - previous_network_latency_ms_);

        // 记录最近一次 jitter。
        udp_receive_stats_.jitter_last_ms = jitter_ms;

        // jitter 样本数量 +1。
        ++udp_receive_stats_.jitter_samples;

        // 更新 jitter 平均值。
        //
        // 这是增量平均算法：
        // avg = avg + (new_value - avg) / sample_count
        //
        // 好处是不需要保存所有历史 jitter 样本。
        udp_receive_stats_.jitter_avg_ms +=
                (jitter_ms - udp_receive_stats_.jitter_avg_ms) /
                static_cast<double>(udp_receive_stats_.jitter_samples);

        // 如果当前 jitter 超过历史最大值，就更新最大 jitter。
        if (jitter_ms > udp_receive_stats_.jitter_max_ms) {
            udp_receive_stats_.jitter_max_ms = jitter_ms;
        }

        // 计算分片丢失比例。
        //
        // total_frag = 收到的分片 + 超时丢失的分片
        const std::uint64_t total_frag = udp_receive_stats_.fragments_received + udp_receive_stats_.timed_out_fragments;

        // 如果 total_frag 为 0，丢包率就是 0。
        // 否则：
        // loss_pct = 超时分片数 / 总分片数 * 100%
        const double loss_pct = total_frag == 0 ? 0.0
                : static_cast<double>(udp_receive_stats_.timed_out_fragments) * 100.0 / static_cast<double>(total_frag);

        // 取出 jitter buffer 已输出帧数。
        const std::uint64_t total_emitted = udp_receive_stats_.jitter_buffer_emitted_frames;

        // 计算 jitter buffer 跳帧比例。
        //
        // skip_pct = 被跳过帧数 / 已输出帧数 * 100%
        const double skip_pct = total_emitted == 0 ? 0.0
                : static_cast<double>(udp_receive_stats_.jitter_buffer_skipped_frames) * 100.0 / static_cast<double>(total_emitted);

        // 把 jitter、丢包率、跳帧率喂给 adaptive_jitter_。
        //
        // adaptive_jitter_ 会根据网络情况决定：
        // 1. 当前网络质量如何
        // 2. jitter buffer 目标延迟是多少
        // 3. 最大等待时间是多少
        // 4. 最多缓存多少帧
        adaptive_jitter_.RecordJitter(jitter_ms, loss_pct, skip_pct, now_ns);

        // 从 adaptive_jitter_ 读取 P50 jitter。
        //
        // P50 可以理解成中位数：
        // 50% 的 jitter 样本小于等于这个值。
        udp_receive_stats_.jitter_p50_ms = adaptive_jitter_.jitter_p50_ms();

        // 从 adaptive_jitter_ 读取 P95 jitter。
        //
        // P95 表示：
        // 95% 的 jitter 样本小于等于这个值。
        // 它比平均值更能反映偶发的大抖动。
        udp_receive_stats_.jitter_p95_ms = adaptive_jitter_.jitter_p95_ms();

        // 记录当前自适应 jitter buffer 的模式名称。
        udp_receive_stats_.jitter_buffer_active_mode = adaptive_jitter_.active_mode_name();

        // 记录当前网络质量名称。
        //
        // 例如可能是 good / fair / poor 之类。
        udp_receive_stats_.jitter_buffer_quality = adaptive_jitter_.network_quality_name();
    }

    // 保存当前帧网络延迟，供下一帧计算 jitter 使用。
    previous_network_latency_ms_ = network_latency_ms;

    // 标记已经有上一帧网络延迟样本。
    has_previous_network_latency_ = true;
}

// 更新 jitter buffer 深度统计。
//
// 深度就是当前 jitter buffer 里缓存了多少帧。
void StreamClient::UpdateJitterBufferDepthStats() {
    // 当前深度 = jitter buffer 中的帧数量。
    udp_receive_stats_.jitter_buffer_current_depth = udp_jitter_buffer_.size();

    // 如果当前深度超过历史最大深度，就更新最大深度。
    if (udp_receive_stats_.jitter_buffer_current_depth > udp_receive_stats_.jitter_buffer_max_depth) {
        udp_receive_stats_.jitter_buffer_max_depth = udp_receive_stats_.jitter_buffer_current_depth;
    }
}

// 记录某一帧在 jitter buffer 中等待了多久。
//
// wait_ns 表示等待时间，单位纳秒。
void StreamClient::RecordJitterBufferWait(std::uint64_t wait_ns) {
    // 把纳秒转换成毫秒。
    const double wait_ms = static_cast<double>(wait_ns) / 1000000.0;

    // 记录最近一次等待时间。
    udp_receive_stats_.jitter_buffer_wait_last_ms = wait_ms;

    // 等待时间样本数 +1。
    ++udp_receive_stats_.jitter_buffer_wait_samples;

    // 更新平均等待时间。
    //
    // 仍然使用增量平均算法。
    udp_receive_stats_.jitter_buffer_wait_avg_ms +=
            (wait_ms - udp_receive_stats_.jitter_buffer_wait_avg_ms) /
            static_cast<double>(udp_receive_stats_.jitter_buffer_wait_samples);

    // 更新最大等待时间。
    if (wait_ms > udp_receive_stats_.jitter_buffer_wait_max_ms) {
        udp_receive_stats_.jitter_buffer_wait_max_ms = wait_ms;
    }
}

}  // namespace sclient
