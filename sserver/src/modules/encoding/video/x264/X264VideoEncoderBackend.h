#ifndef SSERVER_MODULES_ENCODING_VIDEO_X264_X264VIDEOENCODERBACKEND_H
#define SSERVER_MODULES_ENCODING_VIDEO_X264_X264VIDEOENCODERBACKEND_H

#include <memory>
#include <string>

#include "modules/encoding/video/VideoEncoderBackend.h"
#include "x264.h"

namespace sserver {
namespace modules {
namespace encoding {

// 创建 x264 视频编码后端对象
std::unique_ptr<VideoEncoderBackend> CreateX264VideoEncoderBackend();

// x264 视频编码后端实现类
class X264VideoEncoderBackend final : public VideoEncoderBackend {
public:
    X264VideoEncoderBackend();
    ~X264VideoEncoderBackend() override;

    // 初始化 x264 编码器
    bool Initialize(int width, int height, int fps, const config::CodecConfig &config, std::string *error_message) override;

    // 编码一帧 YUYV422 原始图像
    bool EncodeYuyv422Frame(
            const std::uint8_t *input,
            std::size_t input_length,
            std::vector<std::uint8_t> *output,
            bool *is_keyframe,
            std::string *error_message) override;

     // 关闭 x264 编码器并释放资源
    void Shutdown() override;

    // 返回当前编码后端类型
    EncodeBackend backend() const override;

    // 返回当前编码后端名称
    const std::string &backend_name() const override;

private:
    // 编码后端名称
    std::string backend_name_;

    // x264 编码参数结构体
    x264_param_t *param_;

    // x264 编码器句柄
    x264_t *handle_;

    // x264 输入图像结构
    x264_picture_t *picture_;

    // x264 输出 NALU 指针
    x264_nal_t *nal_;

    // 输入帧的显示时间戳 / 帧序号
    int pts_;
};

}  // namespace encoding
}  // namespace modules
}  // namespace sserver

#endif  // SSERVER_MODULES_ENCODING_VIDEO_X264_X264VIDEOENCODERBACKEND_H
