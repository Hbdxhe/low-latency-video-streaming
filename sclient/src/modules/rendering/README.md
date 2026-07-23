# src/modules/rendering

## 1. 模块定位

视频渲染模块。负责将解码后的 YUV 像素数据渲染到屏幕窗口，叠加 HUD 信息面板（延迟统计、队列深度、jitter buffer 状态等）。采用"统一接口 + 后端工厂"架构，当前仅实现 OpenGL 渲染后端。

## 2. 核心职责

- 创建 OpenGL 窗口（GLFW）
- 将 YUV420P/NV12 帧数据上传到 GPU 纹理
- 通过着色器进行 YUV→RGB 色彩空间转换
- 渲染全屏四边形显示视频画面
- 使用 ImGui 叠加 HUD 信息面板
- 支持全屏切换、截图、暂停等交互
- 计算并显示实时 FPS

## 3. 主要文件说明

| 文件 | 作用 |
|---|---|
| `VideoRenderer.h` | 渲染器统一接口。定义 `Initialize()`、`Render()`、`PollKey()` 等方法，以及 `RenderFrameInfo` 结构体 |
| `VideoRenderer.cpp` | 渲染器工厂：根据 `RenderBackend` 创建对应后端 |
| `VideoRendererBackend.h` | 渲染后端抽象基类 |
| `opengl/OpenGlVideoRendererBackend.h` | OpenGL 后端工厂函数声明 |
| `opengl/OpenGlVideoRendererBackend.cpp` | OpenGL 渲染实现：窗口管理、纹理上传、着色器编译、HUD 渲染 |
| `opengl/OpenGlVideoRendererHud.h` | HUD 面板接口声明 |
| `opengl/OpenGlVideoRendererHud.cpp` | HUD 面板实现：延迟统计、队列深度、jitter buffer 状态、连接状态 |
| `opengl/ImGuiOpenGL3Backend.cpp` | ImGui OpenGL3 后端桥接（包含 `imgui_impl_opengl3.cpp`） |
| `opengl/shaders/video_renderer.vert` | 顶点着色器（位置 + 纹理坐标 + 缩放） |
| `opengl/shaders/video_renderer.frag` | 片段着色器（YUV→RGB 色彩转换，支持 YUV420P 和 NV12） |

## 4. 核心类 / 函数说明

### VideoRenderer

作用：
- 渲染器统一接口
- 被 `main.cpp` 主线程调用
- 内部持有 `VideoRendererBackend` 的具体实现

关键函数：
- `Initialize(title, backend, vsync, error, info)`：初始化渲染器
- `Render(frame, frame_info, error)`：渲染一帧
- `PollKey(delay_ms)`：轮询键盘输入，返回按键码
- `UpdateWindowTitle(title)`：更新窗口标题
- `ToggleFullscreen()`：切换全屏
- `SaveScreenshot(path, error)`：保存截图
- `Shutdown()`：释放渲染资源
- `SupportsNativeFrame(frame)`：检查帧格式是否支持

### RenderFrameInfo

作用：
- 渲染帧信息结构体
- 由 `main.cpp` 填充，传给 `Render()` 和 HUD

关键字段：
- `transport`：传输协议
- `frame_width` / `frame_height`：帧尺寸
- `capture_to_render` / `network_to_receive` / `receive_to_decode` / `decode_time` / `decode_to_render` / `receive_to_render`：各阶段延迟统计
- `receive_queue_depth` / `decode_queue_depth`：队列深度
- `udp_jitter_*`：jitter buffer 状态
- `udp_nack_enabled` / `udp_fec_enabled` / `udp_nack_requests_sent` / `udp_fec_recovered_frames`：NACK/FEC 状态
- `connected` / `waiting_for_first_frame`：连接状态
- `fragment_loss_percent`：丢包率

### OpenGlVideoRendererBackend

作用：
- OpenGL 渲染后端实现
- 使用 GLFW 创建窗口，GLAD 加载 GL 函数
- 使用 ImGui 渲染 HUD
- 使用 PBO 加速纹理上传

关键流程：
1. `Initialize()`：glfwInit → 创建窗口 → gladLoadGL → 编译着色器 → 创建 VAO/VBO/EBO → 创建纹理 → 初始化 ImGui
2. `Render()`：计算 FPS → 上传纹理（PBO 优先） → 设置缩放 → 绘制四边形 → ImGui NewFrame → 渲染 HUD → SwapBuffers
3. `PollKey()`：glfwPollEvents → 检测按键（H 切换 HUD、Space 暂停、F 全屏、S 截图、R 重置统计）
4. `SaveScreenshot()`：glReadPixels → stbi_write_png
5. `Shutdown()`：销毁 ImGui → 删除 GL 资源 → 销毁窗口 → glfwTerminate

关键设计：
- 使用 2 个 PBO（Pixel Buffer Object）交替上传纹理，减少 CPU 等待
- 支持 YUV420P（3 纹理）和 NV12（2 纹理）两种格式
- 着色器中通过 `u_color_mode` uniform 切换色彩转换模式
- 窗口缩放保持视频宽高比

### RenderOpenGlHudPanel()

作用：
- 渲染 ImGui HUD 面板
- 显示连接状态、传输协议、分辨率、FPS
- 显示队列深度、丢包率
- 显示端到端延迟（capture→render、network→recv）
- 显示本地延迟（recv→render、recv→decode、decode time、decode→render）
- 显示 jitter buffer 状态（策略、模式、质量、深度、目标延迟、抖动 p50/p95）
- 显示 NACK/FEC 状态
- 显示快捷键提示

### 着色器

顶点着色器（`video_renderer.vert`）：
- 输入：位置 + 纹理坐标
- uniform：`u_scale`（缩放因子，用于保持宽高比）
- 输出：纹理坐标传递给片段着色器

片段着色器（`video_renderer.frag`）：
- uniform：`u_plane0`/`u_plane1`/`u_plane2`（Y/U/V 或 Y/UV 纹理）、`u_color_mode`
- `u_color_mode=1`：YUV420P → 从 3 个纹理采样 Y/U/V → 转 RGB
- `u_color_mode=2`：NV12 → 从 2 个纹理采样 Y 和 UV → 转 RGB
- 默认：BGR 直通

## 5. 数据流说明

输入：
- `DecodedFrame`：解码后的帧（YUV420P 或 NV12）
- `RenderFrameInfo`：帧信息（延迟统计、队列状态等）

处理：
- 像素格式判断 → 选择纹理上传策略
- PBO 映射 → 逐行拷贝（处理 linesize padding） → glTexImage2D/glTexSubImage2D
- 着色器 YUV→RGB 转换
- ImGui 渲染 HUD

输出：
- GLFW 窗口中的视频画面 + HUD 叠加
- 截图文件（PNG）

## 6. 与其他模块的关系

```
modules/rendering/
  ├── 输入：DecodedFrame（来自 modules/decoding）
  ├── 输入：RenderFrameInfo（来自 app/main.cpp）
  ├── 使用 common/media/DecodedFrame.h 的帧结构
  ├── 使用 common/metrics/LatencyStats.h 的 LatencySummary
  ├── 使用 common/log/Logger 输出日志
  ├── 依赖外部库：libavcodec（像素格式定义）、glfw3、gl
  └── 依赖第三方：glad、imgui、stb_image_write
```

## 7. 线程模型 / 队列模型

本模块当前不直接管理线程。所有渲染操作在主线程（渲染线程）中执行。

线程安全：
- OpenGL 上下文绑定到主线程，不跨线程访问
- `RenderFrameInfo` 由主线程构造和消费

## 8. 配置参数

| 参数 | 默认值 | 说明 |
|---|---|---|
| `--renderer` | `auto` | 渲染后端：auto/opengl |
| `--vsync` | `off` | 是否开启垂直同步 |
| `--window-title` | `sclient` | 窗口标题 |

## 9. 调试建议

- **窗口创建失败**：检查 `glfwInit()` 和 `glfwCreateWindow()` 返回值，确认 OpenGL 3.3 可用
- **着色器编译失败**：查看日志中 `failed to compile vertex/fragment shader` 的错误信息
- **纹理上传问题**：如果花屏，检查 linesize 是否正确（可能含对齐 padding）
- **HUD 不显示**：按 H 键切换 HUD 显示
- **性能问题**：检查 PBO 上传是否生效，观察 FPS 显示
- **适合打断点的位置**：
  - `OpenGlVideoRendererBackend::Render()`：观察每帧渲染
  - `UploadPlaneTexture()`：观察纹理上传
  - `RenderOpenGlHudPanel()`：观察 HUD 数据

## 10. 后续扩展方向

- 增加 Vulkan 渲染后端
- 增加 Metal 渲染后端（macOS）
- 增加更多像素格式支持（BGR、P010）
- 增加画面缩放/平移交互
- 增加 OSD 文字叠加
- 增加多窗口支持
- 增加录制功能（编码输出）
