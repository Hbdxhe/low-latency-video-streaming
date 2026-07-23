#ifndef SSERVER_MODULES_ENCODING_VIDEO_VIDEOENCODERBACKEND_H
#define SSERVER_MODULES_ENCODING_VIDEO_VIDEOENCODERBACKEND_H

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "config/AppConfig.h"
#include "modules/encoding/video/VideoEncoder.h"

namespace sserver {
namespace modules {
namespace encoding {

class VideoEncoderBackend {
public:
    virtual ~VideoEncoderBackend() = default;

    // 初始化编码器
    virtual bool Initialize(int width, int height, int fps, const config::CodecConfig &config, std::string *error_message) = 0;

    // 编码一帧 YUYV422 原始图像，当前项目的编码输入格式是 YUYV422，
    // 但很多 H.264 编码器通常要求输入 I420 / YUV420P
    //
    // 因此具体后端内部通常会执行：
    //
    //      YUYV422 原始帧
    //              ↓
    //      转换为 I420 / YUV420P
    //              ↓
    //      调用具体编码器，例如 x264
    //              ↓
    //      输出 H.264 payload
    virtual bool EncodeYuyv422Frame(
            const std::uint8_t *input,
            std::size_t input_length,
            std::vector<std::uint8_t> *output,
            bool *is_keyframe,
            std::string *error_message) = 0;

     // 关闭编码器并释放内部资源
    virtual void Shutdown() = 0;

    // 返回当前编码后端类型
    virtual EncodeBackend backend() const = 0;

    // 返回当前编码后端名称
    virtual const std::string &backend_name() const = 0;
};

}  // namespace encoding
}  // namespace modules
}  // namespace sserver

#endif  // SSERVER_MODULES_ENCODING_VIDEO_VIDEOENCODERBACKEND_H
