#ifndef SSERVER_COMMON_NET_H264ANNEXB_H
#define SSERVER_COMMON_NET_H264ANNEXB_H

#include <cstddef>
#include <cstdint>
#include <vector>

namespace sserver {
namespace common {
namespace net {

// 一个不拥有内存的 H.264 NALU（Network Abstraction Layer Unit）视图。
//
// H.264 编码结果由多个 NALU 组成，例如：
//   SPS：序列参数集，描述分辨率、profile 等全局解码参数；
//   PPS：图像参数集，描述熵编码等图像级参数；
//   IDR slice：可作为随机访问点的关键图像数据；
//   non-IDR slice：通常依赖其他参考帧的普通图像数据。
//
// data 和 size 只描述原始 Annex-B vector 中的一段连续内存：
//   - 不包含 00 00 01 / 00 00 00 01 起始码；
//   - data 指向 NAL header 的第一个字节；
//   - size 包含 NAL header 和其后的 RBSP/EBSP 数据。
//
// 该结构不分配、不复制、也不释放 NALU 数据，因此拆分速度快。但调用者必须
// 保证原始 vector 在所有 H264NaluView 使用期间仍然存在，并且不能进行可能
// 导致 vector 重新分配的修改，否则 data 会成为悬空指针。
struct H264NaluView {
    const std::uint8_t *data = nullptr;
    std::size_t size = 0;

    // 从单字节 H.264 NAL header 中提取 nal_unit_type。
    //
    // NAL header 的位布局：
    //   +---+------+-----------------+
    //   | F | NRI  | nal_unit_type   |
    //   +---+------+-----------------+
    //     1    2           5 bit
    //
    // 因此使用 0x1F（二进制 00011111）保留低 5 bit。
    // 常见类型：1=非 IDR slice，5=IDR slice，7=SPS，8=PPS。
    // 无有效数据时返回 0；NAL type 0 本身也是未指定类型。
    std::uint8_t nal_type() const {
        if (data == nullptr || size == 0) {
            return 0;
        }
        return static_cast<std::uint8_t>(data[0] & 0x1F);
    }
};

// 从一段字节数据里找下一个 start code
inline std::size_t FindAnnexBStartCode(
        const std::vector<std::uint8_t> &data,
        std::size_t offset,
        std::size_t *prefix_size) {
    // 从 offset 开始扫描 Annex-B 起始码。
    //
    // Annex-B 使用起始码划分相邻 NALU，允许两种形式：
    //   00 00 01
    //   00 00 00 01
    //
    // 返回值是起始码第一个 0 的下标；prefix_size 输出起始码长度 3 或 4。
    // 找不到时返回 data.size()，并将 prefix_size 置 0。
    //
    // prefix_size 允许为 nullptr，适用于调用者只关心位置的场景。
    if (prefix_size != nullptr) {
        *prefix_size = 0;
    }

    // 最短起始码有 3 字节，因此只有剩余空间至少为 3 时才继续检查。
    for (std::size_t index = offset; index + 3 <= data.size(); ++index) {
        // 起始码前两个字节必须都是 0，先做快速过滤。
        if (data[index] != 0 || data[index + 1] != 0) {
            continue;
        }
        if (data[index + 2] == 1) {
            // 匹配三字节起始码 00 00 01。
            if (prefix_size != nullptr) {
                *prefix_size = 3;
            }
            return index;
        }
        if (index + 4 <= data.size() && data[index + 2] == 0 && data[index + 3] == 1) {
            // 匹配四字节起始码 00 00 00 01。
            if (prefix_size != nullptr) {
                *prefix_size = 4;
            }
            return index;
        }
    }

    return data.size();
}

// 把一整段 Annex-B H.264 数据拆成多个 NALU
inline void SplitAnnexBNalus(const std::vector<std::uint8_t> &data, std::vector<H264NaluView> *output) {
    // 将一段 Annex-B 字节流拆成多个 NALU 视图。
    //
    // 例如：
    //   00 00 00 01 [SPS] 00 00 00 01 [PPS] 00 00 01 [IDR]
    //
    // 会得到三个 H264NaluView，分别指向 SPS、PPS 和 IDR。这个过程不复制
    // NALU payload，只保存指针和长度，所以返回的视图依赖 data 的生命周期。
    if (output == nullptr) {
        return;
    }
    // 输出参数表示“本次完整拆分结果”，调用前先清除旧内容。
    output->clear();
    std::size_t start_code_size = 0;
    std::size_t start = FindAnnexBStartCode(data, 0, &start_code_size);
    while (start < data.size()) {
        // 跳过 3/4 字节起始码后，payload_start 指向 NAL header。
        const std::size_t payload_start = start + start_code_size;
        std::size_t next_start_code_size = 0;
        const std::size_t next = FindAnnexBStartCode(data, payload_start, &next_start_code_size);
        std::size_t payload_end = next;
        // Annex-B 允许在 NALU 后、下一个起始码前存在 trailing_zero_8bits。
        // 这些补零属于字节流格式边界，不是当前 NALU 内容，因此从尾部剔除。
        while (payload_end > payload_start && data[payload_end - 1] == 0) {
            --payload_end;
        }
        if (payload_end > payload_start) {
            // 忽略两个起始码之间没有实际 NALU 字节的空片段。
            H264NaluView view;
            view.data = data.data() + payload_start;
            view.size = payload_end - payload_start;
            output->push_back(view);
        }
        start = next;
        start_code_size = next_start_code_size;
    }
}

inline std::vector<H264NaluView> SplitAnnexBNalus(const std::vector<std::uint8_t> &data) {
    // 返回 vector 的便捷重载，适合不需要复用输出容器的调用位置。
    // 返回的仍然是非拥有视图，并没有复制 data 中的 NALU 字节。
    std::vector<H264NaluView> nalus;
    SplitAnnexBNalus(data, &nalus);
    return nalus;
}

// 往输出 buffer 追加固定的 4 字节 start code
inline void AppendAnnexBStartCode(std::vector<std::uint8_t> *output) {
    // 向输出字节流追加四字节 Annex-B 起始码 00 00 00 01。
    //
    // 三字节和四字节起始码都合法；这里统一输出四字节形式，使接收端从
    // RTP 裸 NALU/FU-A 分片恢复 Annex-B 时获得稳定一致的格式。
    if (output == nullptr) {
        return;
    }

    output->push_back(0);
    output->push_back(0);
    output->push_back(0);
    output->push_back(1);
}

// 把一个裸 NALU 重新拼成 Annex-B 格式
inline void AppendAnnexBNalu(
        const std::uint8_t *data,
        std::size_t size,
        std::vector<std::uint8_t> *output) {
    // 将一个不带起始码的裸 NALU 追加为 Annex-B 格式：
    //
    //   00 00 00 01 + NAL header + NAL payload
    //
    // 常用于客户端完成 RTP H.264 解包以后，把单 NALU 或重组后的 FU-A
    // 恢复为 FFmpeg 解码器容易接受的 Annex-B 字节流。
    if (data == nullptr || size == 0 || output == nullptr) {
        return;
    }

    AppendAnnexBStartCode(output);
    output->insert(output->end(), data, data + size);
}

}  // namespace net
}  // namespace common
}  // namespace sserver

#endif  // SSERVER_COMMON_NET_H264ANNEXB_H
