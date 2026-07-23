#ifndef SCLIENT_COMMON_NET_H264ANNEXB_H
#define SCLIENT_COMMON_NET_H264ANNEXB_H

#include <cstddef>
#include <cstdint>
#include <vector>

namespace sclient {
namespace common {
namespace net {

/**
 * H.264 Annex B 格式工具函数
 *
 * Annex B 是 H.264 码流的一种封装格式，使用 0x00000001 或 0x000001 作为 NAL 单元的起始码。
 */

/** 追加 Annex B 起始码（0x00000001） */
inline void AppendAnnexBStartCode(std::vector<std::uint8_t> *output) {
    if (output == nullptr) {
        return;
    }

    output->push_back(0);
    output->push_back(0);
    output->push_back(0);
    output->push_back(1);
}

/**
 * 追加一个 NAL 单元（带起始码）
 *
 * @param data NAL 单元数据（不含起始码）
 * @param size 数据大小
 * @param output 输出缓冲区
 */
inline void AppendAnnexBNalu(
        const std::uint8_t *data,
        std::size_t size,
        std::vector<std::uint8_t> *output) {
    if (data == nullptr || size == 0 || output == nullptr) {
        return;
    }

    AppendAnnexBStartCode(output);
    output->insert(output->end(), data, data + size);
}

/**
 * 检测码流中是否包含 IDR 帧的 NAL 单元
 *
 * IDR 帧（NAL type = 5）是关键帧，解码器可以从 IDR 帧开始独立解码。
 * 常用于判断是否可以开始解码或切换流。
 *
 * @param data Annex B 格式的码流数据
 * @param size 数据大小
 * @return 包含 IDR NAL 单元返回 true
 */
inline bool HasIdrNalUnit(const std::uint8_t *data, std::size_t size) {
    if (data == nullptr || size < 4) {
        return false;
    }

    // 扫描起始码（0x000001 或 0x00000001）
    for (std::size_t i = 0; i + 3 < size; ++i) {
        if (data[i] == 0 && data[i + 1] == 0) {
            std::size_t nal_offset = 0;
            if (data[i + 2] == 1) {
                nal_offset = i + 3;
            } else if (i + 4 < size && data[i + 2] == 0 && data[i + 3] == 1) {
                nal_offset = i + 4;
            } else {
                continue;
            }

            // 检查 NAL 类型：低5位为5表示 IDR 帧
            if (nal_offset < size) {
                const std::uint8_t nal_type = data[nal_offset] & 0x1F;
                if (nal_type == 5) {
                    return true;
                }
            }
        }
    }
    return false;
}

}  // namespace net
}  // namespace common
}  // namespace sclient

#endif  // SCLIENT_COMMON_NET_H264ANNEXB_H
