#ifndef SCLIENT_UI_RENDERING_OPENGL_OPENGLVIDEORENDERERHUD_H
#define SCLIENT_UI_RENDERING_OPENGL_OPENGLVIDEORENDERERHUD_H

#include "modules/rendering/VideoRenderer.h"

namespace sclient {

/**
 * 渲染 OpenGL HUD 面板
 *
 * 在视频画面上叠加显示延迟统计、网络状态等信息
 *
 * @param frame_info 帧信息（包含各种统计数据）
 * @param fps 当前帧率
 * @param hud_visible 输入输出：HUD 是否可见
 */
void RenderOpenGlHudPanel(const RenderFrameInfo &frame_info, double fps, bool *hud_visible);

}  // namespace sclient

#endif  // SCLIENT_UI_RENDERING_OPENGL_OPENGLVIDEORENDERERHUD_H
