#include "modules/rendering/VideoRenderer.h"

#include <utility>

#include "modules/rendering/VideoRendererBackend.h"
#include "modules/rendering/opengl/OpenGlVideoRendererBackend.h"

namespace sclient {

namespace {

// 根据请求的渲染后端类型，创建真正的渲染 backend。
//
// requested_backend：用户希望使用哪种渲染方式。
// error_message：如果创建失败，用于返回错误信息。
std::unique_ptr<VideoRendererBackend> CreateVideoRendererBackend(
        RenderBackend requested_backend,
        std::string *error_message) {
    switch (requested_backend) {
        case RenderBackend::kAuto:
        case RenderBackend::kOpenGl:
            return CreateOpenGlVideoRendererBackend();
        default:
            if (error_message != nullptr) {
                *error_message = "requested renderer backend is not supported";
            }
            return nullptr;
    }
}

}  // namespace

struct VideoRendererImpl {
    // 用户请求的渲染后端
    RenderBackend requested_backend = RenderBackend::kAuto;

    // 当前实际使用的渲染后端
    RenderBackend active_backend = RenderBackend::kOpenGl;

    // 当前实际后端名称
    std::string active_backend_name = "opengl";

    // 真正的渲染后端对象
    std::unique_ptr<VideoRendererBackend> backend;
};

VideoRenderer::VideoRenderer()
        : impl_(std::make_unique<VideoRendererImpl>()) {
}

VideoRenderer::~VideoRenderer() {
    Shutdown();
}

// 初始化渲染器。
//
// window_title：窗口标题。
// requested_backend：希望使用的渲染后端，比如 auto / opengl。
// enable_vsync：是否开启垂直同步。
// error_message：初始化失败时返回错误信息。
// info_message：初始化成功时返回渲染器信息。
bool VideoRenderer::Initialize(
        const std::string &window_title,
        RenderBackend requested_backend,
        bool enable_vsync,
        std::string *error_message,
        std::string *info_message) {
    Shutdown();

    // 记录用户请求的渲染后端类型
    impl_->requested_backend = requested_backend;

    // 根据 requested_backend 创建具体渲染后端
    impl_->backend = CreateVideoRendererBackend(requested_backend, error_message);
    if (!impl_->backend) {
        return false;
    }

    // 初始化具体后端
    if (!impl_->backend->Initialize(window_title, enable_vsync, error_message)) {
        impl_->backend.reset();
        return false;
    }

    impl_->active_backend = impl_->backend->backend();
    impl_->active_backend_name = impl_->backend->backend_name();

    // 如果调用者希望拿到初始化信息
    if (info_message != nullptr) {
        // 生成一段信息字符串。
        //
        // 如果后端名是 opengl，就显示更详细的：
        // opengl(glfw+glad+imgui)
        //
        // glfw：创建窗口和处理输入
        // glad：加载 OpenGL 函数
        // imgui：绘制 HUD / 调试 UI
        *info_message = std::string("renderer=") +
                (impl_->active_backend_name == "opengl" ? "opengl(glfw+glad+imgui)" : impl_->active_backend_name) +
                " vsync=" + (enable_vsync ? "on" : "off");
    }
    return true;
}

// 轮询键盘输入
int VideoRenderer::PollKey(int delay_ms) const {
    if (!impl_->backend) {
        // 返回 27。
        // 27 通常是 ESC 键的 ASCII 码。
        // 这相当于告诉主循环：可以退出
        return 27;
    }
    return impl_->backend->PollKey(delay_ms);
}

// 返回当前实际使用的渲染后端枚举。
RenderBackend VideoRenderer::backend() const {
    return impl_->active_backend;
}

// 返回当前实际使用的渲染后端名称。
const std::string &VideoRenderer::backend_name() const {
    return impl_->active_backend_name;
}

// 判断当前 backend 是否支持直接渲染这个 DecodedFrame。
//
// 为什么需要这个函数？
// 因为不同 renderer 可能支持不同像素格式。
// 例如 OpenGL backend 可能支持 YUV420P / NV12，
// 但不支持 unknown 格式。
bool VideoRenderer::SupportsNativeFrame(const DecodedFrame &frame) const {
    return impl_->backend != nullptr && impl_->backend->SupportsNativeFrame(frame);
}

// 渲染一帧解码后的视频图像。
//
// frame：解码模块输出的 DecodedFrame，里面有 YUV 数据指针。
// frame_info：这一帧的附加信息，比如延迟统计、HUD 信息等。
// error_message：失败时返回错误信息。
bool VideoRenderer::Render(const DecodedFrame &frame, const RenderFrameInfo &frame_info, std::string *error_message) {
    if (!impl_->backend) {
        if (error_message != nullptr) {
            *error_message = "renderer is not initialized";
        }
        return false;
    }

    // 把真正的渲染工作交给具体 backend。
    //
    // 对 OpenGL 来说，里面会做：
    // 1. 上传 Y/U/V 或 NV12 纹理
    // 2. 设置 shader 参数
    // 3. 绘制全屏矩形
    // 4. 绘制 HUD
    // 5. swap buffers 显示到窗口
    return impl_->backend->Render(frame, frame_info, error_message);
}

// 更新窗口标题
void VideoRenderer::UpdateWindowTitle(const std::string &title) {
    if (impl_->backend) {
        impl_->backend->UpdateWindowTitle(title);
    }
}

// 切换全屏 / 窗口模式
void VideoRenderer::ToggleFullscreen() {
    if (impl_->backend) {
        impl_->backend->ToggleFullscreen();
    }
}

// 保存当前窗口画面截图。
//
// path：截图保存路径。
// error_message：失败时返回错误信息。
bool VideoRenderer::SaveScreenshot(const std::string &path, std::string *error_message) {
    if (!impl_->backend) {
        if (error_message != nullptr) {
            *error_message = "renderer is not initialized";
        }
        return false;
    }
    return impl_->backend->SaveScreenshot(path, error_message);
}

// 关闭渲染器，释放资源。
void VideoRenderer::Shutdown() {
    if (impl_->backend) {
        // 先调用 backend 的 Shutdown。
        //
        // OpenGL backend 会在这里释放：
        // 1. 纹理
        // 2. shader program
        // 3. VAO/VBO
        // 4. ImGui 资源
        // 5. GLFW 窗口和 context
        impl_->backend->Shutdown();
        impl_->backend.reset();
    }

    impl_->requested_backend = RenderBackend::kAuto;
    impl_->active_backend = RenderBackend::kOpenGl;
    impl_->active_backend_name = "opengl";
}

}  // namespace sclient
