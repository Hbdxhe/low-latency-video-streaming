#ifndef SCLIENT_UI_RENDERING_OPENGL_OPENGLVIDEORENDERERBACKEND_H
#define SCLIENT_UI_RENDERING_OPENGL_OPENGLVIDEORENDERERBACKEND_H

#include <memory>

#include "modules/rendering/VideoRendererBackend.h"

namespace sclient {

/** 创建 OpenGL 视频渲染器后端实例 */
std::unique_ptr<VideoRendererBackend> CreateOpenGlVideoRendererBackend();

}  // namespace sclient

#endif  // SCLIENT_UI_RENDERING_OPENGL_OPENGLVIDEORENDERERBACKEND_H
