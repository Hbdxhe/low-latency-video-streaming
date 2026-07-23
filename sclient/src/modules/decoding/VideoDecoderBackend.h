#ifndef SCLIENT_SERVICES_DECODING_VIDEODECODERBACKEND_H
#define SCLIENT_SERVICES_DECODING_VIDEODECODERBACKEND_H

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>

#include "common/media/DecodedFrame.h"
#include "modules/decoding/VideoDecoder.h"

namespace sclient {

/**
 * 视频解码器后端抽象接口
 *
 * 定义解码器后端的通用接口，支持不同的解码实现（如 FFmpeg 软件解码）
 */
class VideoDecoderBackend {
public:
    virtual ~VideoDecoderBackend() = default;

    /** 初始化解码器 */
    virtual bool Initialize(std::string *error_message) = 0;
    /** 解码一帧数据 */
    virtual bool Decode(
            const std::uint8_t *data,
            std::size_t size,
            DecodedFrame *decoded_frame,
            std::string *error_message) = 0;
    /** 关闭解码器 */
    virtual void Shutdown() = 0;
    /** 获取后端名称 */
    virtual const std::string &backend_name() const = 0;
};

}  // namespace sclient

#endif  // SCLIENT_SERVICES_DECODING_VIDEODECODERBACKEND_H
