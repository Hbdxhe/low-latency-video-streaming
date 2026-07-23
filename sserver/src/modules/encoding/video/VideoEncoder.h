#ifndef SSERVER_MODULES_ENCODING_VIDEO_VIDEOENCODER_H
#define SSERVER_MODULES_ENCODING_VIDEO_VIDEOENCODER_H

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "config/AppConfig.h"

namespace sserver {
namespace modules {
namespace encoding {

class VideoEncoderBackend;

// 编码后端类型枚举
enum class EncodeBackend {
    kAuto,
    kX264,
};

// 编码后端解析结果
struct VideoEncoderBackendSelection {
    EncodeBackend backend = EncodeBackend::kAuto;
    std::string backend_name = "auto";
};

// 根据配置文件中的 codec.backend 字段解析编码后端
inline bool ResolveVideoEncoderBackendSelection(
        const config::CodecConfig &config,
        VideoEncoderBackendSelection *selection,
        std::string *error_message) {

    // selection 是输出参数，必须有效。
    // 如果传入 nullptr，函数无法返回解析结果，因此直接失败。
    if (selection == nullptr) {
        if (error_message != nullptr) {
            *error_message = "video encoder backend selection output is null";
        }
        return false;
    }

    // codec.backend 不能为空
    if (config.backend.empty()) {
        if (error_message != nullptr) {
            *error_message = "codec.backend must not be empty";
        }
        return false;
    }

    if (config.backend == "x264") {
        selection->backend = EncodeBackend::kX264;
        selection->backend_name = "x264";
        return true;
    }

    // 其他 backend 配置均视为非法。
    if (error_message != nullptr) {
        if (config.backend == "libx264") {
            *error_message = "codec.backend=libx264 is no longer supported; use codec.backend=x264 for software encoding";
        } else {
            *error_message = "codec.backend must be 'x264'";
        }
    }
    return false;
}

// 视频编码后端工厂，该工厂根据 VideoEncoderBackendSelection 创建具体编码后端实例。
class VideoEncoderBackendFactory {
public:
    static std::unique_ptr<VideoEncoderBackend> Create(
            const VideoEncoderBackendSelection &selection,
            std::string *error_message);
};

// 视频编码器对外统一封装类
class VideoEncoder {
public:
    VideoEncoder();
    ~VideoEncoder();

    bool Initialize(int width, int height, int fps, const config::CodecConfig &config);
    bool Initialize(int width, int height, int fps, const config::CodecConfig &config, std::string *error_message);
    bool EncodeYuyv422Frame(
            const std::uint8_t *input,
            std::size_t input_length,
            std::vector<std::uint8_t> *output,
            bool *is_keyframe);

    // 编码一帧 YUYV422 原始图像
    bool EncodeYuyv422Frame(
            const std::uint8_t *input,
            std::size_t input_length,
            std::vector<std::uint8_t> *output,
            bool *is_keyframe,
            std::string *error_message);

    // 关闭编码器并释放内部资源
    void Shutdown();

    // 返回当前实际使用的编码后端类型
    EncodeBackend backend() const;

    // 返回当前实际使用的编码后端名称
    const std::string &backend_name() const;

private:
    // PImpl 内部实现结构体声明，这里只做前向声明，具体成员变量和实现逻辑放在 .cpp 中。
    //
    // 这样做的好处：
    // 1. 隐藏 x264 等第三方库细节；
    // 2. 减少头文件依赖；
    // 3. 修改内部成员时减少重新编译；
    // 4. 对外提供更稳定的类接口
    struct VideoEncoderImpl;
    std::unique_ptr<VideoEncoderImpl> impl_;
};

// 为 VideoEncoder 起一个接口别名
using IVideoEncoder = VideoEncoder;

// 编码器工厂。
//
// 上层可以通过 CodecFactory::Create(config) 创建编码器实例，而不是直接 new VideoEncoder。
class CodecFactory {
public:
    static std::unique_ptr<IVideoEncoder> Create(const config::CodecConfig &config);
};

}  // namespace encoding
}  // namespace modules
}  // namespace sserver

#endif  // SSERVER_MODULES_ENCODING_VIDEO_VIDEOENCODER_H
