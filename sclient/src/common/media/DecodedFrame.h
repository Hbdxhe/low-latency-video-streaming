#ifndef SCLIENT_DECODEDFRAME_H
#define SCLIENT_DECODEDFRAME_H

#include <array>
#include <cstdint>
#include <memory>

namespace sclient {

/** 解码后的像素格式 */
enum class DecodedPixelFormat {
    kUnknown = 0,  /**< 未知格式 */
    kYuv420p,      /**< YUV 4:2:0 平面格式 */
    kNv12,         /**< NV12 半平面格式 */
};

/**
 * 解码后的视频帧
 *
 * 包含帧的尺寸、像素格式和各平面数据指针。
 * 数据内存由owner共享指针管理生命周期。
 */
struct DecodedFrame {
    int width = 0;   /**< 帧宽度（像素） */
    int height = 0;  /**< 帧高度（像素） */
    DecodedPixelFormat pixel_format = DecodedPixelFormat::kUnknown;
    /** 各平面数据指针（YUV格式通常使用前3个） */
    std::array<const std::uint8_t *, 4> data = {{nullptr, nullptr, nullptr, nullptr}};
    /** 各平面每行字节数 */
    std::array<int, 4> linesize = {{0, 0, 0, 0}};
    std::shared_ptr<void> owner;  /**< 管理数据内存的共享指针 */

    /** 检查帧是否为空（无有效数据） */
    bool empty() const {
        return width <= 0 || height <= 0 || data[0] == nullptr;
    }
};

}  // namespace sclient

#endif  // SCLIENT_DECODEDFRAME_H
