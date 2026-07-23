#ifndef SCLIENT_VIDEODECODER_H
#define SCLIENT_VIDEODECODER_H

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>

#include "common/media/DecodedFrame.h"

namespace sclient {

/** 解码后端类型 */
enum class DecodeBackend {
    kAuto,      /**< 自动选择（当前仅支持软件解码） */
    kSoftware,  /**< 软件解码（FFmpeg） */
};

struct VideoDecoderImpl;  /**< 解码器实现（Pimpl 模式） */

/**
 * H.264 视频解码器
 *
 * 使用 FFmpeg 进行软件解码，将 H.264 码流解码为 YUV 像素数据
 */
class VideoDecoder {
public:
    VideoDecoder();
    ~VideoDecoder();

    /** 初始化解码器（使用默认后端） */
    bool Initialize(std::string *error_message);
    /** 初始化解码器（指定后端） */
    bool Initialize(DecodeBackend requested_backend, std::string *error_message);
    /** 解码一帧 H.264 数据 */
    bool Decode(const std::uint8_t *data, std::size_t size, DecodedFrame *decoded_frame, std::string *error_message);
    /** 关闭解码器 */
    void Shutdown();

private:
    std::unique_ptr<VideoDecoderImpl> impl_;
};

}  // namespace sclient

#endif  // SCLIENT_VIDEODECODER_H
