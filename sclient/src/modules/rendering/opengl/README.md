# src/modules/rendering/opengl

## 1. 模块定位

OpenGL 渲染后端实现。使用 GLFW 创建窗口，GLAD 加载 GL 函数，ImGui 渲染 HUD 面板，是当前唯一的渲染后端。

## 2. 核心职责

- 创建和管理 OpenGL 窗口（GLFW）
- 编译和管理 GLSL 着色器程序
- 将 YUV 帧数据上传到 GPU 纹理（支持 PBO 加速）
- 通过着色器进行 YUV→RGB 色彩空间转换
- 渲染全屏四边形显示视频
- 使用 ImGui 渲染 HUD 信息面板
- 支持全屏切换、截图、键盘交互

## 3. 主要文件说明

| 文件 | 作用 |
|---|---|
| `OpenGlVideoRendererBackend.h` | 工厂函数声明：`CreateOpenGlVideoRendererBackend()` |
| `OpenGlVideoRendererBackend.cpp` | OpenGL 渲染后端完整实现（~920 行） |
| `OpenGlVideoRendererHud.h` | HUD 面板接口：`RenderOpenGlHudPanel()` |
| `OpenGlVideoRendererHud.cpp` | HUD 面板实现：延迟统计、jitter buffer 状态、连接状态 |
| `ImGuiOpenGL3Backend.cpp` | ImGui OpenGL3 后端桥接（包含 imgui 头文件和实现） |
| `shaders/video_renderer.vert` | 顶点着色器 |
| `shaders/video_renderer.frag` | 片段着色器 |

## 4. 核心类 / 函数说明

### OpenGlVideoRendererBackend

作用：
- `VideoRendererBackend` 的具体实现
- 管理整个 OpenGL 渲染管线

关键成员：
- `window_`：GLFW 窗口句柄
- `texture_ids_[3]`：YUV 三平面纹理
- `pixel_unpack_buffer_ids_[3][2]`：PBO 双缓冲
- `shader_program_id_`：着色器程序
- `vertex_array_id_` / `vertex_buffer_id_` / `element_buffer_id_`：VAO/VBO/EBO

关键函数：
- `Initialize()`：初始化 GLFW → 创建窗口 → 加载 GLAD → 编译着色器 → 创建几何体 → 初始化 ImGui
- `Render()`：计算 FPS → 上传纹理 → 设置缩放 → 绘制 → ImGui HUD → SwapBuffers
- `UploadFrameTextures()`：根据像素格式上传 YUV 纹理（YUV420P: 3 纹理, NV12: 2 纹理）
- `UploadPlaneTexture()`：单平面纹理上传，优先使用 PBO
- `PollKey()`：轮询键盘输入
- `ToggleFullscreen()`：切换全屏
- `SaveScreenshot()`：glReadPixels → stbi_write_png
- `Shutdown()`：销毁 ImGui → 删除 GL 资源 → 销毁窗口 → glfwTerminate

### RenderOpenGlHudPanel()

作用：
- 渲染 ImGui HUD 面板
- 显示内容：
  - 连接状态（Connected/Disconnected/Waiting）
  - 传输协议、分辨率、FPS
  - 队列深度（recv/decode）
  - 丢包率
  - 端到端延迟（capture→render, network→recv）
  - 本地延迟（recv→render, recv→decode, decode time, decode→render）
  - Jitter Buffer 状态（策略、模式、质量、深度、目标延迟、抖动 p50/p95、等待时间、跳过/丢弃帧数）
  - NACK/FEC 状态
  - 快捷键提示

### 着色器

顶点着色器：
- 输入：`a_position`（位置）、`a_tex_coord`（纹理坐标）
- uniform：`u_scale`（缩放，用于保持宽高比）
- 输出：`v_tex_coord`

片段着色器：
- uniform：`u_plane0`/`u_plane1`/`u_plane2`、`u_color_mode`
- `u_color_mode=1`：YUV420P，3 纹理采样 → YUV→RGB
- `u_color_mode=2`：NV12，2 纹理采样 → YUV→RGB
- 默认：BGR 直通

## 5. 数据流说明

输入：
- `DecodedFrame`：解码后的帧数据
- `RenderFrameInfo`：帧信息和统计

处理：
- PBO 映射 → 逐行拷贝（处理 linesize padding） → glTexImage2D/glTexSubImage2D
- 着色器 YUV→RGB 转换
- ImGui 渲染 HUD

输出：
- GLFW 窗口画面
- 截图文件（PNG）

## 6. 与其他模块的关系

- 实现 `VideoRendererBackend` 接口
- 被 `VideoRenderer` 通过工厂函数创建
- 使用 `common/media/DecodedFrame.h` 的帧数据
- 使用 `common/metrics/LatencyStats.h` 的 `LatencySummary`
- 使用 `common/log/Logger` 输出日志
- 依赖：glad、imgui、stb_image_write、glfw3、gl

## 7. 线程模型 / 队列模型

本模块当前不直接管理线程。所有 GL 操作在主线程中执行。

## 8. 配置参数

| 参数 | 默认值 | 说明 |
|---|---|---|
| `--renderer` | `auto` | 选择渲染后端 |
| `--vsync` | `off` | 垂直同步 |
| `--window-title` | `sclient` | 窗口标题 |

## 9. 调试建议

- **窗口创建失败**：确认 OpenGL 3.3 可用，检查 GLFW 和 GLAD
- **着色器编译失败**：查看日志中的着色器错误信息
- **黑屏**：检查纹理上传是否成功，检查 `DecodedFrame.empty()`
- **花屏**：检查 linesize 是否正确
- **HUD 不显示**：按 H 键切换
- **性能**：检查 PBO 是否生效，观察 FPS

## 10. 后续扩展方向

- 增加 Vulkan 后端
- 增加更多像素格式
- 增加画面缩放/平移
- 增加 OSD 叠加
