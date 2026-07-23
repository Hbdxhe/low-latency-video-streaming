#ifndef SCLIENT_UI_RENDERING_VIDEORENDERERBACKEND_H
#define SCLIENT_UI_RENDERING_VIDEORENDERERBACKEND_H

#include <memory>
#include <string>

#include "modules/rendering/VideoRenderer.h"

namespace sclient {

/**
 * 视频渲染器后端抽象接口
 *
 * 定义渲染器后端的通用接口，支持不同的渲染实现（如 OpenGL）
 */
class VideoRendererBackend {
public:
    virtual ~VideoRendererBackend() = default;

    /** 初始化渲染器 */
    virtual bool Initialize(const std::string &window_title, bool enable_vsync, std::string *error_message) = 0;
    /** 检查是否支持该帧格式 */
    virtual bool SupportsNativeFrame(const DecodedFrame &frame) const = 0;
    /** 渲染一帧 */
    virtual bool Render(const DecodedFrame &frame, const RenderFrameInfo &frame_info, std::string *error_message) = 0;
    /** 轮询键盘事件 */
    virtual int PollKey(int delay_ms) = 0;
    /** 更新窗口标题 */
    virtual void UpdateWindowTitle(const std::string &title) = 0;
    /** 切换全屏模式 */
    virtual void ToggleFullscreen() = 0;
    /** 保存截图 */
    virtual bool SaveScreenshot(const std::string &path, std::string *error_message) = 0;
    /** 关闭渲染器 */
    virtual void Shutdown() = 0;
    /** 获取后端类型 */
    virtual RenderBackend backend() const = 0;
    /** 获取后端名称 */
    virtual const std::string &backend_name() const = 0;
};

}  // namespace sclient

#endif  // SCLIENT_UI_RENDERING_VIDEORENDERERBACKEND_H
