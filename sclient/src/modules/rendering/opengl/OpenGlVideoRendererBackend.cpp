#include "modules/rendering/opengl/OpenGlVideoRendererBackend.h"

// glad 的作用是加载 OpenGL 函数指针。
#include <glad/glad.h>
// GLFW 主要负责创建窗口、创建 OpenGL 上下文、处理键盘鼠标事件
#include <GLFW/glfw3.h>

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <string>
#include <thread>
#include <vector>

#include "imgui/imgui.h"
#include "imgui/backends/imgui_impl_glfw.h"
#include "imgui/backends/imgui_impl_opengl3.h"
#include "common/log/Logger.h"
#include "modules/rendering/opengl/OpenGlVideoRendererHud.h"

#define STB_IMAGE_WRITE_IMPLEMENTATION
#include "stb/stb_image_write.h"

namespace sclient {

namespace {

// 像素上传缓冲数量。
//
// 这里是 2，表示可能使用双缓冲思路上传像素，
// 避免 CPU 写入和 GPU 读取同一块缓冲造成等待。
const std::size_t kPixelUploadBufferCount = 2;

// 内嵌顶点 shader 源码。
//
// shader 是运行在 GPU 上的小程序。
// 顶点 shader 负责处理顶点位置和纹理坐标。
const char *kEmbeddedVertexShaderSource =
        "#version 330 core\n"
        "\n"
        "layout (location = 0) in vec2 a_position;\n"
        "layout (location = 1) in vec2 a_tex_coord;\n"
        "\n"
        "uniform vec2 u_scale;\n"
        "\n"
        "out vec2 v_tex_coord;\n"
        "\n"
        "void main() {\n"
        "    gl_Position = vec4(a_position * u_scale, 0.0, 1.0);\n"
        "    v_tex_coord = a_tex_coord;\n"
        "}\n";

// 内嵌片段 shader 源码。
//
// fragment shader 负责给屏幕上的每个像素计算颜色。
// 这里最核心的事是：把 YUV 转成 RGB。
const char *kEmbeddedFragmentShaderSource =
        "#version 330 core\n"
        "\n"
        "in vec2 v_tex_coord;\n"
        "\n"
        "uniform sampler2D u_plane0;\n"
        "uniform sampler2D u_plane1;\n"
        "uniform sampler2D u_plane2;\n"
        "uniform int u_color_mode;\n"
        "\n"
        "out vec4 frag_color;\n"
        "\n"
        "vec3 yuv_to_rgb(float y, float u, float v) {\n"
        "    float u_shifted = u - 0.5;\n"
        "    float v_shifted = v - 0.5;\n"
        "    return vec3(\n"
        "        y + 1.402 * v_shifted,\n"
        "        y - 0.344136 * u_shifted - 0.714136 * v_shifted,\n"
        "        y + 1.772 * u_shifted\n"
        "    );\n"
        "}\n"
        "\n"
        "void main() {\n"
        "    if (u_color_mode == 1) {\n"
        "        float y = texture(u_plane0, v_tex_coord).r;\n"
        "        float u = texture(u_plane1, v_tex_coord).r;\n"
        "        float v = texture(u_plane2, v_tex_coord).r;\n"
        "        frag_color = vec4(yuv_to_rgb(y, u, v), 1.0);\n"
        "        return;\n"
        "    }\n"
        "\n"
        "    if (u_color_mode == 2) {\n"
        "        float y = texture(u_plane0, v_tex_coord).r;\n"
        "        vec2 uv = texture(u_plane1, v_tex_coord).rg;\n"
        "        frag_color = vec4(yuv_to_rgb(y, uv.x, uv.y), 1.0);\n"
        "        return;\n"
        "    }\n"
        "\n"
        "    frag_color = vec4(texture(u_plane0, v_tex_coord).rgb, 1.0);\n"
        "}\n";

// 原生颜色模式枚举。
// 这个枚举用于告诉 shader 当前上传的纹理是什么格式。
enum class NativeColorMode {
    kBgr = 0,
    kYuv420p = 1,
    kNv12 = 2,
};

// 获取当前单调时间，单位纳秒。
//
// 单调时间不会因为系统时间调整而倒退，适合做耗时统计。
std::uint64_t MonotonicNowNs() {
    const auto now = std::chrono::steady_clock::now().time_since_epoch();
    return static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(now).count());
}

// 去掉字符串末尾的空白字符。
//
// 主要用于 shader 编译错误日志。
// OpenGL 返回的 log 末尾可能有换行或空格。
std::string TrimTrailingWhitespace(const std::string &value) {
    std::size_t end = value.size();

    // 从后往前删掉 \n、\r、空格
    while (end > 0 && (value[end - 1] == '\n' || value[end - 1] == '\r' || value[end - 1] == ' ')) {
        --end;
    }
    return value.substr(0, end);
}


// 加载 shader 源码。
//
// 当前项目这里使用内嵌 shader，
// 也就是 shader 代码直接写在这个 .cpp 字符串里。
// 没有从外部文件读取。
bool LoadShaderSources(
        std::string *vertex_source,    // 输出：顶点 shader 源码
        std::string *fragment_source,  // 输出：片段 shader 源码
        std::string *resolved_path,    // 输出：shader 来源路径
        std::string *error_message) {  // 输出：错误信息
    if (vertex_source == nullptr || fragment_source == nullptr) {
        if (error_message != nullptr) {
            *error_message = "shader source output pointers are null";
        }
        return false;
    }

    // 使用内嵌顶点 shader
    *vertex_source = kEmbeddedVertexShaderSource;

    // 使用内嵌片段 shader
    *fragment_source = kEmbeddedFragmentShaderSource;

    // 如果调用者想知道 shader 从哪里加载
    if (resolved_path != nullptr) {
        *resolved_path = "(embedded)";
    }
    return true;
}

// 根据解码帧的像素格式，决定 OpenGL shader 使用哪种颜色模式
bool ResolveNativeColorMode(DecodedPixelFormat pixel_format, NativeColorMode *mode) {
    if (mode == nullptr) {
        return false;
    }

    // 根据项目自己的 DecodedPixelFormat 判断
    switch (pixel_format) {
        case DecodedPixelFormat::kYuv420p:
            *mode = NativeColorMode::kYuv420p;
            return true;
        case DecodedPixelFormat::kNv12:
            *mode = NativeColorMode::kNv12;
            return true;
        default:
            *mode = NativeColorMode::kBgr;
            return false;
    }
}

// 编译一个 shader。
// shader 可能是顶点 shader，也可能是片段 shader。
bool CompileShader(GLenum shader_type, const std::string &source, GLuint *shader_id, std::string *error_message) {
    if (shader_id == nullptr) {
        if (error_message != nullptr) {
            *error_message = "shader output pointer is null";
        }
        return false;
    }

    // 创建 OpenGL shader 对象。
    //
    // shader_type 可能是：
    // GL_VERTEX_SHADER：顶点 shader
    // GL_FRAGMENT_SHADER：片段 shader
    *shader_id = glCreateShader(shader_type);
    if (*shader_id == 0) {
        if (error_message != nullptr) {
            *error_message = "glCreateShader failed";
        }
        return false;
    }

    // 取出 shader 源码 C 字符串指针
    const char *source_data = source.c_str();

    // 把源码交给 OpenGL。
    //
    // 参数含义：
    // shader_id：要设置源码的 shader
    // 1：源码字符串数量
    // &source_data：源码内容
    // nullptr：让 OpenGL 自己通过 '\0' 判断字符串长度
    glShaderSource(*shader_id, 1, &source_data, nullptr);

    // 编译 shader
    glCompileShader(*shader_id);

    // 准备保存编译状态。
    GLint compile_status = GL_FALSE;

    // 查询 shader 是否编译成功
    glGetShaderiv(*shader_id, GL_COMPILE_STATUS, &compile_status);
    if (compile_status == GL_TRUE) {
        return true;
    }

    // 如果走到这里，说明 shader 编译失败。
    // 先查询错误日志长度
    GLint log_length = 0;

    // 获取 shader 信息日志长度
    glGetShaderiv(*shader_id, GL_INFO_LOG_LENGTH, &log_length);

    // 创建日志缓冲区。
    // std::max(1, log_length) 保证至少有 1 个字符空间。
    std::vector<char> log_buffer(static_cast<std::size_t>(std::max(1, log_length)));

    // 读取 shader 编译错误日志
    glGetShaderInfoLog(*shader_id, log_length, nullptr, log_buffer.data());

    // 如果调用者需要错误信息
    if (error_message != nullptr) {
        const std::string stage = shader_type == GL_VERTEX_SHADER ? "vertex" : "fragment";
        *error_message = "failed to compile " + stage + " shader: " + TrimTrailingWhitespace(std::string(log_buffer.data()));
    }

    // 删除编译失败的 shader 对象，避免资源泄漏
    glDeleteShader(*shader_id);

    // 把 shader_id 置 0，表示无效
    *shader_id = 0;
    return false;
}

// 创建完整 shader program。
//
// shader program = 顶点 shader + 片段 shader 链接后的 GPU 程序。
// 渲染视频时会使用这个 program。
bool CreateShaderProgram(
        GLuint *program_id,                  // 输出：OpenGL shader program ID
        GLint *scale_uniform_location,       // 输出：u_scale uniform 位置
        GLint *color_mode_uniform_location,  // 输出：u_color_mode uniform 位置
        std::string *resolved_shader_path,   // 输出：shader 来源
        std::string *error_message) {        // 输出：错误信息

    if (program_id == nullptr || scale_uniform_location == nullptr || color_mode_uniform_location == nullptr) {
        if (error_message != nullptr) {
            *error_message = "shader program output pointers are null";
        }
        return false;
    }

    // 准备保存顶点 shader 源码
    std::string vertex_shader_source;

    // 准备保存片段 shader 源码
    std::string fragment_shader_source;

    // 加载 shader 源码
    if (!LoadShaderSources(&vertex_shader_source, &fragment_shader_source, resolved_shader_path, error_message)) {
        return false;
    }

    // 顶点 shader 对象 ID
    GLuint vertex_shader_id = 0;

    // 片段 shader 对象 ID
    GLuint fragment_shader_id = 0;

    // 编译顶点 shader
    if (!CompileShader(GL_VERTEX_SHADER, vertex_shader_source, &vertex_shader_id, error_message)) {
        return false;
    }

    // 编译片段 shader
    if (!CompileShader(GL_FRAGMENT_SHADER, fragment_shader_source, &fragment_shader_id, error_message)) {
        // 如果片段 shader 编译失败，要删除已经成功编译的顶点 shader
        glDeleteShader(vertex_shader_id);
        return false;
    }

    // 创建 shader program
    *program_id = glCreateProgram();
    if (*program_id == 0) {
        if (error_message != nullptr) {
            *error_message = "glCreateProgram failed";
        }

        // 清理两个 shader
        glDeleteShader(vertex_shader_id);
        glDeleteShader(fragment_shader_id);
        return false;
    }

    // 把顶点 shader 挂到 program 上
    glAttachShader(*program_id, vertex_shader_id);
    // 把片段 shader 挂到 program 上
    glAttachShader(*program_id, fragment_shader_id);

    // 链接 program。
    //
    // 链接成功后，顶点 shader 和片段 shader 才能一起工作
    glLinkProgram(*program_id);

    // shader 已经 attach 并 link 过了，
    // 后面不再需要单独保留 shader 对象，可以删除。
    glDeleteShader(vertex_shader_id);

    // 删除片段 shader 对象。
    glDeleteShader(fragment_shader_id);

    // 准备保存 program 链接状态
    GLint link_status = GL_FALSE;

    // 查询链接是否成功
    glGetProgramiv(*program_id, GL_LINK_STATUS, &link_status);

    // 如果链接失败
    if (link_status != GL_TRUE) {
        // 查询错误日志长度
        GLint log_length = 0;

        // 获取 program 链接日志长度
        glGetProgramiv(*program_id, GL_INFO_LOG_LENGTH, &log_length);

        // 创建日志缓冲区
        std::vector<char> log_buffer(static_cast<std::size_t>(std::max(1, log_length)));

        // 读取 program 链接错误日志
        glGetProgramInfoLog(*program_id, log_length, nullptr, log_buffer.data());

        // 如果调用者需要错误信息
        if (error_message != nullptr) {
            *error_message = "failed to link shader program: " + TrimTrailingWhitespace(std::string(log_buffer.data()));
        }

        // 删除失败的 program
        glDeleteProgram(*program_id);
        *program_id = 0;
        return false;
    }

    // 使用这个 shader program
    glUseProgram(*program_id);

    // 设置 u_plane0 这个 sampler 使用纹理单元 0。
    //
    // 后面渲染时会把 Y 平面或 RGB 图绑定到 GL_TEXTURE0
    glUniform1i(glGetUniformLocation(*program_id, "u_plane0"), 0);

    // 设置 u_plane1 使用纹理单元 1。
    //
    // YUV420P 时是 U 平面，NV12 时是 UV 平面。
    glUniform1i(glGetUniformLocation(*program_id, "u_plane1"), 1);

    // 设置 u_plane2 使用纹理单元 2。
    //
    // YUV420P 时是 V 平面
    glUniform1i(glGetUniformLocation(*program_id, "u_plane2"), 2);

    // 获取 u_scale uniform 的位置，后面每帧根据窗口和视频比例设置
    *scale_uniform_location = glGetUniformLocation(*program_id, "u_scale");

    // 获取 u_color_mode uniform 的位置，后面告诉 shader 当前是 YUV420P/NV12/RGB。
    *color_mode_uniform_location = glGetUniformLocation(*program_id, "u_color_mode");
    return true;
}

// 配置一个 OpenGL 2D 纹理的采样参数。
//
// texture_id 是 OpenGL 生成的纹理对象 ID。
// 可以理解为 GPU 里一张图片的编号。
void ConfigureTexture(GLuint texture_id) {
    // 绑定这个纹理。
    //
    // 后面对 GL_TEXTURE_2D 的设置都会作用到 texture_id 这张纹理上。
    glBindTexture(GL_TEXTURE_2D, texture_id);

    // 设置纹理缩小时的过滤方式。
    //
    // GL_LINEAR 表示线性过滤。
    // 画面缩小时会做平滑采样，而不是生硬取最近像素。
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);

    // 设置纹理放大时的过滤方式。
    //
    // 比如 640x480 视频放大到窗口 1280x960，
    // GL_LINEAR 会让画面更平滑。
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);

    // 设置纹理横向坐标越界时怎么处理。
    //
    // GL_CLAMP_TO_EDGE 表示超过边缘时使用边缘像素。
    // 这样可以避免画面边缘出现重复纹理或奇怪颜色。
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);

    // 设置纹理纵向坐标越界时怎么处理
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
}

// 上传一个图像平面到 OpenGL 纹理。
//
// 这个函数的作用就是：
// 把 CPU 内存里的某个 plane 上传到 GPU 纹理
void UploadPlaneTexture(
       GLuint texture_id,                                  // 要上传到哪张 OpenGL 纹理
        int plane_index,                                    // 绑定到第几个纹理单元，比如 0/1/2
        int width,                                          // 这个平面的宽度
        int height,                                         // 这个平面的高度
        const std::uint8_t *data,                           // CPU 内存中的平面数据指针
        int linesize,                                       // 每一行占多少字节
        GLint internal_format,                              // OpenGL 内部保存格式，比如 GL_R8 / GL_RG8
        GLenum format,                                      // 输入数据格式，比如 GL_RED / GL_RG
        GLuint (&pixel_unpack_buffer_ids)[2],               // PBO ID 数组，用于异步上传
        std::size_t (&pixel_unpack_buffer_sizes)[2],        // 每个 PBO 当前大小
        std::size_t &next_pixel_unpack_buffer_index,        // 下次使用哪个 PBO
        int &cached_width,                                  // 缓存的纹理宽度
        int &cached_height) {                               // 缓存的纹理高度
    if (data == nullptr || width <= 0 || height <= 0 || linesize <= 0) {
        return;
    }

    // 判断每个像素占几个字节。
    //
    // 如果 format == GL_RG，说明一个像素有 R/G 两个通道，比如 NV12 的 UV 平面。
    // 那么每个像素 2 字节。
    //
    // 否则，比如 GL_RED，只有一个通道，每像素 1 字节。
    const std::size_t bytes_per_pixel = format == GL_RG ? 2U : 1U;

    // 一行真正要上传的有效字节数。
    //
    // 注意：这里用 width * bytes_per_pixel，
    // 不直接用 linesize。
    //
    // 因为 linesize 可能包含 FFmpeg 为内存对齐添加的 padding。
    const std::size_t packed_row_bytes = static_cast<std::size_t>(width) * bytes_per_pixel;

    // 整个平面紧凑存储时需要的字节数
    const std::size_t buffer_size = packed_row_bytes * static_cast<std::size_t>(height);

    // 当前使用哪个 PBO。
    //
    // kPixelUploadBufferCount 是 2，所以这里在 0 和 1 之间轮换。
    const std::size_t pbo_index = next_pixel_unpack_buffer_index % kPixelUploadBufferCount;

    // 取出当前 PBO 的 OpenGL ID
    const GLuint pbo_id = pixel_unpack_buffer_ids[pbo_index];

    // 激活纹理单元。
    //
    // plane_index = 0 -> GL_TEXTURE0
    // plane_index = 1 -> GL_TEXTURE1
    // plane_index = 2 -> GL_TEXTURE2
    glActiveTexture(GL_TEXTURE0 + plane_index);

    // 绑定目标纹理
    glBindTexture(GL_TEXTURE_2D, texture_id);

    // 设置像素解包对齐为 1。
    //
    // 默认 OpenGL 可能按 4 字节对齐读像素，
    // 但 YUV 平面一行不一定是 4 的倍数。
    // 设置为 1 可以避免行读取错位。
    glPixelStorei(GL_UNPACK_ALIGNMENT, 1);

    // 标记是否成功通过 PBO 上传。
    bool uploaded_with_pbo = false;

    // 如果 PBO 存在，并且 buffer_size 合法，就尝试使用 PBO 上传
    if (pbo_id != 0 && buffer_size > 0) {
        // 绑定 PBO。
        //
        // GL_PIXEL_UNPACK_BUFFER 表示：
        // 后续 glTexImage2D / glTexSubImage2D 的数据来源可以来自这个 buffer。
        glBindBuffer(GL_PIXEL_UNPACK_BUFFER, pbo_id);

        // 为 PBO 分配 buffer_size 大小的 GPU/驱动缓冲区。
        //
        // nullptr 表示只分配，不上传初始数据。
        // GL_STREAM_DRAW 表示这个 buffer 会频繁更新、用于传给 GPU
        glBufferData(GL_PIXEL_UNPACK_BUFFER, static_cast<GLsizeiptr>(buffer_size), nullptr, GL_STREAM_DRAW);

        // 记录当前 PBO 的大小
        pixel_unpack_buffer_sizes[pbo_index] = buffer_size;

        // 把 PBO 映射到 CPU 可写地址
        void *mapped_buffer = glMapBufferRange(
                GL_PIXEL_UNPACK_BUFFER,
                0,
                static_cast<GLsizeiptr>(buffer_size),
                GL_MAP_WRITE_BIT | GL_MAP_INVALIDATE_BUFFER_BIT);

        // 如果映射成功
        if (mapped_buffer != nullptr) {
            // 转成 uint8_t*，方便按字节写入
            std::uint8_t *target = reinterpret_cast<std::uint8_t *>(mapped_buffer);

            // 逐行拷贝 plane 数据到 PBO
            for (int row = 0; row < height; ++row) {
                std::memcpy(
                        target + static_cast<std::size_t>(row) * packed_row_bytes,
                        data + static_cast<std::size_t>(row) * static_cast<std::size_t>(linesize),
                        packed_row_bytes);
            }

            // 取消映射，让 OpenGL 知道 CPU 已经写完了
            if (glUnmapBuffer(GL_PIXEL_UNPACK_BUFFER) == GL_TRUE) {
                // PBO 中的数据已经是紧凑排列，所以 row length 设为 0
                glPixelStorei(GL_UNPACK_ROW_LENGTH, 0);

                // 如果纹理尺寸变了。
                //
                // 比如第一帧是 1280x720，或者后续分辨率变化了
                if (cached_width != width || cached_height != height) {
                    cached_width = width;
                    cached_height = height;

                    // 重新分配纹理存储，并从当前绑定的 PBO 上传数据。
                    //
                    // 最后一个参数 nullptr 的意思是：
                    // 数据不来自 CPU 指针，而是来自当前 GL_PIXEL_UNPACK_BUFFER。
                    glTexImage2D(GL_TEXTURE_2D, 0, internal_format, width, height, 0, format, GL_UNSIGNED_BYTE, nullptr);

                // 如果纹理尺寸没变
                } else {
                    // 只更新纹理内容，不重新分配纹理
                    glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, width, height, format, GL_UNSIGNED_BYTE, nullptr);
                }
                uploaded_with_pbo = true;
            }
        }
    }

    // 如果 PBO 上传失败，走普通上传路径
    if (!uploaded_with_pbo) {
        // 解绑 PBO。
        //
        // 这样 glTexImage2D / glTexSubImage2D 的最后一个参数会被当成 CPU 指针。
        glBindBuffer(GL_PIXEL_UNPACK_BUFFER, 0);

        // 告诉 OpenGL 源数据每行有多少“像素”。
        //
        // linesize 是字节数。
        // GL_UNPACK_ROW_LENGTH 要的是像素数。
        //
        // 所以要除以 bytes_per_pixel。
        glPixelStorei(GL_UNPACK_ROW_LENGTH, linesize / std::max<int>(1, static_cast<int>(bytes_per_pixel)));

        // 如果纹理尺寸变化
        if (cached_width != width || cached_height != height) {
            cached_width = width;
            cached_height = height;
            glTexImage2D(GL_TEXTURE_2D, 0, internal_format, width, height, 0, format, GL_UNSIGNED_BYTE, data);

        // 如果尺寸没变。
        } else {
            glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, width, height, format, GL_UNSIGNED_BYTE, data);
        }
    }

    // 上传结束后，解绑 PBO，避免影响其他 OpenGL 操作。
    glBindBuffer(GL_PIXEL_UNPACK_BUFFER, 0);

    // 恢复 row length 默认值。
    glPixelStorei(GL_UNPACK_ROW_LENGTH, 0);

    // 下次换另一个 PBO。
    //
    // 双 PBO 轮换可以减少 CPU/GPU 互相等
    next_pixel_unpack_buffer_index = (pbo_index + 1) % kPixelUploadBufferCount;
}


// 初始化 ImGui 的 UI 风格。
//
// ImGui 在这个项目里用来画 HUD 信息，
// 比如 FPS、延迟、网络状态、丢包、jitter buffer 等。
void InitializeImGuiStyle() {
    // 使用 ImGui 默认暗色主题
    ImGui::StyleColorsDark();

    // 获取当前 ImGui 样式对象。
    ImGuiStyle &style = ImGui::GetStyle();

    // 设置窗口圆角。
    style.WindowRounding = 10.0f;

    // 设置按钮、输入框等控件圆角。
    style.FrameRounding = 8.0f;

    // 设置 HUD 窗口背景色。
    //
    // RGBA:
    // R=0.07, G=0.09, B=0.12, A=0.88
    // 看起来是半透明深色背景。
    style.Colors[ImGuiCol_WindowBg] = ImVec4(0.07f, 0.09f, 0.12f, 0.88f);

    // 设置普通 header 颜色
    style.Colors[ImGuiCol_Header] = ImVec4(0.12f, 0.48f, 0.32f, 0.85f);

    // 设置鼠标悬停 header 颜色
    style.Colors[ImGuiCol_HeaderHovered] = ImVec4(0.17f, 0.60f, 0.39f, 0.95f);

    // 设置激活状态 header 颜色
    style.Colors[ImGuiCol_HeaderActive] = ImVec4(0.20f, 0.69f, 0.45f, 1.0f);
}


// OpenGL 视频渲染后端。
//
// 它继承 VideoRendererBackend，说明它实现了渲染模块抽象接口。
// final 表示这个类不能再被继承。
class OpenGlVideoRendererBackend final : public VideoRendererBackend {
public:
    ~OpenGlVideoRendererBackend() override {
        Shutdown();
    }

    // 初始化 OpenGL 渲染后端
    //
    // window_title：窗口标题
    // enable_vsync：是否开启垂直同步
    // error_message：失败时输出错误信息
    bool Initialize(const std::string &window_title, bool enable_vsync, std::string *error_message) override {
        Shutdown();

        // 保存窗口标题。
        window_title_ = window_title;

        // 保存是否开启 vsync。
        //
        // vsync 开启时，交换缓冲会等待显示器刷新，
        // 可以减少画面撕裂，但可能增加一点延迟。
        vsync_enabled_ = enable_vsync;

        // 默认显示 HUD。
        hud_visible_ = true;

        // H 键上一次是否按下，用于做按键边沿检测。
        //
        // 防止按住 H 时 HUD 一直疯狂开关。
        h_key_was_down_ = false;

        // 上一次渲染时间清零。
        last_render_timestamp_ns_ = 0;

        // 当前显示 FPS 清零。
        displayed_fps_ = 0.0;

        // 标记窗口还没有根据视频帧尺寸初始化过。
        window_sized_to_frame_ = false;

        // 初始化 GLFW。
        //
        // GLFW 负责窗口、OpenGL context、输入事件。
        if (!glfwInit()) {
            if (error_message != nullptr) {
                *error_message = "glfwInit failed";
            }
            ResetState();
            return false;
        }

        // 标记 GLFW 已初始化
        glfw_initialized_ = true;

        // 设置 OpenGL 主版本号为 3。
        glfwWindowHint(GLFW_CONTEXT_VERSION_MAJOR, 3);

        // 设置 OpenGL 次版本号为 3。
        glfwWindowHint(GLFW_CONTEXT_VERSION_MINOR, 3);

        // 使用 OpenGL Core Profile。
        //
        // Core Profile 是现代 OpenGL 风格，
        // 不使用老式固定管线 API。
        glfwWindowHint(GLFW_OPENGL_PROFILE, GLFW_OPENGL_CORE_PROFILE);

        // 允许窗口调整大小。
        glfwWindowHint(GLFW_RESIZABLE, GLFW_TRUE);

        // 创建窗口。
        //
        // 初始大小 640x480。
        // 后面渲染时会根据 framebuffer 和视频比例适配显示。
        window_ = glfwCreateWindow(640, 480, window_title_.c_str(), nullptr, nullptr);
        if (window_ == nullptr) {
            if (error_message != nullptr) {
                *error_message = "glfwCreateWindow failed for OpenGL 3.3 core profile";
            }
            Shutdown();
            return false;
        }

        // 把刚创建的窗口 OpenGL context 设置为当前线程的 current context。
        //
        // OpenGL API 都作用于当前 context。
        glfwMakeContextCurrent(window_);

        // 设置垂直同步。
        //
        // 1：开启 vsync
        // 0：关闭 vsync
        glfwSwapInterval(vsync_enabled_ ? 1 : 0);

        // 给 GLFW window 绑定 this 指针。
        //
        // 这样 GLFW callback 里可以通过 glfwGetWindowUserPointer 找回当前对象。
        glfwSetWindowUserPointer(window_, this);

        // 设置窗口大小变化回调
        glfwSetWindowSizeCallback(window_, [](GLFWwindow *win, int width, int height) {
            // 从 window user pointer 取回 OpenGlVideoRendererBackend 对象。
            auto *self = static_cast<OpenGlVideoRendererBackend *>(glfwGetWindowUserPointer(win));

            // 如果对象存在，当前不是全屏，并且宽高有效
            if (self != nullptr && !self->is_fullscreen_ && width > 0 && height > 0) {
                self->windowed_width_ = width;
                self->windowed_height_ = height;
            }
        });

        // 加载 OpenGL 函数。
        //
        // gladLoadGLLoader 必须在 OpenGL context 创建并 current 后调用。
        if (gladLoadGLLoader(reinterpret_cast<GLADloadproc>(glfwGetProcAddress)) == 0) {
            if (error_message != nullptr) {
                *error_message = "gladLoadGLLoader failed";
            }
            Shutdown();
            return false;
        }

        // 创建 shader program。
        //
        // 这里会编译顶点 shader、片段 shader，并获取 uniform 位置。
        if (!CreateShaderProgram(
                    &shader_program_id_,
                    &scale_uniform_location_,
                    &color_mode_uniform_location_,
                    nullptr,
                    error_message)) {
            Shutdown();
            return false;
        }

        // 顶点数据。
        //
        // 每个顶点 4 个 float：
        // x, y, u, v
        //
        // x, y 是屏幕坐标，范围 -1 到 1。
        // u, v 是纹理坐标，范围 0 到 1。
        //
        // 这 4 个顶点构成一个矩形。
        const float vertices[] = {
                -1.0f, -1.0f, 0.0f, 1.0f,
                 1.0f, -1.0f, 1.0f, 1.0f,
                 1.0f,  1.0f, 1.0f, 0.0f,
                -1.0f,  1.0f, 0.0f, 0.0f,
        };

        // 索引数据。
        //
        // OpenGL 画三角形。
        // 一个矩形可以拆成两个三角形：
        //
        // 三角形 1：0, 1, 2
        // 三角形 2：2, 3, 0
        const unsigned int indices[] = {
                0, 1, 2,
                2, 3, 0,
        };

        // 创建 VAO，用来记录“顶点属性如何解释”。
        glGenVertexArrays(1, &vertex_array_id_);

        // 创建 VBO，用来保存顶点数据 vertices。
        glGenBuffers(1, &vertex_buffer_id_);

        // 创建 EBO / IBO，element buffer 用来保存 indices。
        glGenBuffers(1, &element_buffer_id_);

        // 创建 3 张纹理。
        //
        // YUV420P 用 3 张：Y、U、V。
        // NV12 用 2 张：Y、UV。
        glGenTextures(3, texture_ids_);

        // 创建 6 个 PBO。
        //
        // 这里是 3 个平面 * 每个平面 2 个 PBO。
        // 用于双缓冲上传像素数据。
        glGenBuffers(6, &pixel_unpack_buffer_ids_[0][0]);

        // 配置第 0 张纹理参数
        ConfigureTexture(texture_ids_[0]);

        // 配置第 1 张纹理参数
        ConfigureTexture(texture_ids_[1]);

        // 配置第 2 张纹理参数
        ConfigureTexture(texture_ids_[2]);

        // 配置第 2 张纹理参数
        ResetPlaneDimensions();

        // 绑定 VAO，后续 VBO/EBO/顶点属性设置会记录到这个 VAO 中
        glBindVertexArray(vertex_array_id_);

        // 绑定 VBO
        glBindBuffer(GL_ARRAY_BUFFER, vertex_buffer_id_);

        // 把顶点数据上传到 VBO，GL_STATIC_DRAW 表示这个顶点数据基本不会变
        glBufferData(GL_ARRAY_BUFFER, sizeof(vertices), vertices, GL_STATIC_DRAW);

        // 绑定 EBO
        glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, element_buffer_id_);

        // 把索引数据上传到 EBO
        glBufferData(GL_ELEMENT_ARRAY_BUFFER, sizeof(indices), indices, GL_STATIC_DRAW);

        // 设置第 0 个顶点属性：a_position。
        //
        // 参数解释：
        // 0：location = 0
        // 2：每个顶点位置有 2 个 float，x 和 y
        // GL_FLOAT：数据类型
        // GL_FALSE：不归一化
        // 4 * sizeof(float)：每个顶点总跨度是 4 个 float
        // 0：位置数据从每个顶点开头开始
        glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 4 * sizeof(float), reinterpret_cast<void *>(0));

        // 启用第 0 个顶点属性
        glEnableVertexAttribArray(0);

        // 设置第 1 个顶点属性：a_tex_coord。
        //
        // 纹理坐标也是 2 个 float，u 和 v。
        // 它从每个顶点的第 3 个 float 开始，也就是偏移 2 * sizeof(float)。
        glVertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, 4 * sizeof(float), reinterpret_cast<void *>(2 * sizeof(float)));

        // 启用第 1 个顶点属性
        glEnableVertexAttribArray(1);

        // 解绑 VAO
        glBindVertexArray(0);

        // 检查 ImGui 版本是否匹配
        IMGUI_CHECKVERSION();

        // 创建 ImGui 上下文
        ImGui::CreateContext();

        // 获取 ImGui IO 对象
        ImGuiIO &io = ImGui::GetIO();

        // 启用键盘导航
        io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;

        // 不使用 ini 文件保存窗口布局
        io.IniFilename = nullptr;

        // 初始化 ImGui 样式
        InitializeImGuiStyle();

        // 初始化 ImGui 的 GLFW 后端。
        if (!ImGui_ImplGlfw_InitForOpenGL(window_, true)) {
            if (error_message != nullptr) {
                *error_message = "ImGui_ImplGlfw_InitForOpenGL failed";
            }
            Shutdown();
            return false;
        }

        // 初始化 ImGui 的 OpenGL3 后端
        if (!ImGui_ImplOpenGL3_Init("#version 330")) {
            if (error_message != nullptr) {
                *error_message = "ImGui_ImplOpenGL3_Init failed";
            }
            Shutdown();
            return false;
        }
        // 标记 ImGui 已初始化
        imgui_initialized_ = true;
        return true;
    }

    // 判断 OpenGL 后端是否支持当前 DecodedFrame 的原生像素格式。
    //
    // 这里的“原生”意思是：不需要先把 YUV 转成 RGB，
    // OpenGL shader 可以直接吃这个格式并显示。
    bool SupportsNativeFrame(const DecodedFrame &frame) const override {
        if (frame.empty()) {
            return false;
        }

        // 默认颜色模式先设为 BGR
        NativeColorMode mode = NativeColorMode::kBgr;

        // 根据 frame.pixel_format 判断是否支持。
        //
        // 当前支持：
        // kYuv420p -> shader 用三个纹理 Y/U/V 转 RGB
        // kNv12    -> shader 用两个纹理 Y/UV 转 RGB
        return ResolveNativeColorMode(frame.pixel_format, &mode);
    }


    // 渲染一帧视频。
    //
    // frame：解码模块输出的 DecodedFrame，里面有 YUV 数据指针。
    // frame_info：这一帧的统计信息，比如延迟、帧序号、网络状态等，用于 HUD。
    // error_message：渲染失败时输出错误信息。
    bool Render(const DecodedFrame &frame, const RenderFrameInfo &frame_info, std::string *error_message) override {
        // 如果 frame 是空的，不能渲染
        if (frame.empty()) {
            if (error_message != nullptr) {
                *error_message = "renderer received an empty decoded frame";
            }
            return false;
        }

        // 如果窗口还没创建
        if (window_ == nullptr) {
            if (error_message != nullptr) {
                *error_message = "native frame rendering requires the OpenGL backend";
            }
            return false;
        }

        // 准备颜色模式，默认先设成 BGR
        NativeColorMode color_mode = NativeColorMode::kBgr;

        // 根据 DecodedFrame 的 pixel_format 判断应该怎么渲染。
        //
        // 如果是 YUV420P：
        //   color_mode = kYuv420p
        //
        // 如果是 NV12：
        //   color_mode = kNv12
        //
        // 如果是不支持的格式：
        //   返回 false
        if (!ResolveNativeColorMode(frame.pixel_format, &color_mode)) {
            if (error_message != nullptr) {
                *error_message = "decoded frame format is not supported for native OpenGL rendering";
            }
            return false;
        }

        const std::uint64_t now_ns = MonotonicNowNs();
        // 如果之前已经渲染过至少一帧，并且当前时间有效
        if (last_render_timestamp_ns_ != 0 && now_ns > last_render_timestamp_ns_) {
            // 计算瞬时 FPS，两帧之间间隔 dt 秒：FPS = 1 / dt
            // 这里 dt 是纳秒，所以用 1,000,000,000 / 纳秒差。
            const double instantaneous_fps = 1000000000.0 / static_cast<double>(now_ns - last_render_timestamp_ns_);

            // 对 FPS 做平滑。
            //
            // 如果 displayed_fps_ 还是 0，说明第一次计算，直接使用 instantaneous_fps。
            //
            // 否则使用指数平滑：新显示 FPS = 旧 FPS * 0.90 + 当前 FPS * 0.10
            // 这样 HUD 上的 FPS 不会跳得太厉害。
            displayed_fps_ = displayed_fps_ == 0.0
                    ? instantaneous_fps
                    : displayed_fps_ * 0.90 + instantaneous_fps * 0.10;
        }
        last_render_timestamp_ns_ = now_ns;

        // 如果窗口还没有记录过初始尺寸
        if (!window_sized_to_frame_) {
            // 获取当前窗口大小
            glfwGetWindowSize(window_, &windowed_width_, &windowed_height_);

            // 标记已经记录过
            window_sized_to_frame_ = true;
        }

        // 把 DecodedFrame 的 YUV 数据上传到 OpenGL 纹理。
        //
        // 这是渲染前最关键的一步：
        // CPU 内存中的 YUV 数据 -> GPU 纹理
        UploadFrameTextures(frame, color_mode);

        // 准备保存 framebuffer 宽度
        int framebuffer_width = 0;

        // 准备保存 framebuffer 高度
        int framebuffer_height = 0;

        // 获取 framebuffer 尺寸。
        //
        // 注意：
        // 窗口尺寸和 framebuffer 尺寸不一定一样。
        // 在高 DPI 屏幕上，framebuffer 可能比窗口逻辑尺寸更大。
        glfwGetFramebufferSize(window_, &framebuffer_width, &framebuffer_height);

        // 设置 OpenGL 视口。
        //
        // 也就是告诉 OpenGL：
        // 接下来绘制结果要映射到 framebuffer 的哪个区域。
        glViewport(0, 0, framebuffer_width, framebuffer_height);

        // 设置清屏颜色。
        //
        // 这里是接近黑色的深灰背景
        glClearColor(0.04f, 0.05f, 0.07f, 1.0f);

        // 清空颜色缓冲区。
        //
        // 相当于先把窗口刷成背景色。
        glClear(GL_COLOR_BUFFER_BIT);

        // 计算 framebuffer 宽高比。
        //
        // 如果高度有效：
        // aspect = width / height
        //
        // 否则兜底为 1.0。
        const double framebuffer_aspect = framebuffer_height > 0
                ? static_cast<double>(framebuffer_width) / static_cast<double>(framebuffer_height)
                : 1.0;

        // 计算视频图像宽高比
        const double image_aspect = frame.height > 0
                ? static_cast<double>(frame.width) / static_cast<double>(frame.height)
                : 1.0;

        // 默认横向缩放比例为 1
        float scale_x = 1.0f;

        // 默认纵向缩放比例为 1
        float scale_y = 1.0f;


        // 如果视频比窗口更“宽”。
        //
        // 举例：
        // 视频 16:9，窗口 4:3。
        //
        // 为了不拉伸视频，需要缩小 y 方向，也就是上下留黑边
        if (image_aspect > framebuffer_aspect) {
            scale_y = static_cast<float>(framebuffer_aspect / image_aspect);

        // 否则说明视频比窗口更“窄”。
        //
        // 举例：
        // 视频 4:3，窗口 16:9。
        //
        // 为了保持比例，需要缩小 x 方向，也就是左右留黑边。
        } else {
            scale_x = static_cast<float>(image_aspect / framebuffer_aspect);
        }

        // 使用前面创建好的 shader program。
        glUseProgram(shader_program_id_);

        // 把缩放比例传给 vertex shader 的 u_scale
        glUniform2f(scale_uniform_location_, scale_x, scale_y);

        // 把颜色模式传给 fragment shader
        glUniform1i(color_mode_uniform_location_, static_cast<int>(color_mode));

        // 绑定 VAO
        glBindVertexArray(vertex_array_id_);

        // 绘制两个三角形组成的视频矩形。
        //
        // GL_TRIANGLES：按三角形绘制
        // 6：一共 6 个索引
        // GL_UNSIGNED_INT：索引类型
        // nullptr：从当前绑定的 EBO 起始位置读取
        glDrawElements(GL_TRIANGLES, 6, GL_UNSIGNED_INT, nullptr);

        // 解绑 VAO
        glBindVertexArray(0);

        // 开启 ImGui OpenGL 新一帧
        ImGui_ImplOpenGL3_NewFrame();

        // 开启 ImGui GLFW 新一帧
        ImGui_ImplGlfw_NewFrame();

        // ImGui 开始构建这一帧 UI
        ImGui::NewFrame();

        RenderFrameInfo hud_info = frame_info;
        hud_info.frame_width = frame.width;
        hud_info.frame_height = frame.height;

        // 绘制 HUD 面板。
        //
        // displayed_fps_ 是当前渲染 FPS。
        // hud_visible_ 控制 HUD 是否显示。
        RenderOpenGlHudPanel(hud_info, displayed_fps_, &hud_visible_);

        // ImGui 完成 UI 构建
        ImGui::Render();

        // 用 OpenGL 绘制 ImGui 的 draw data
        ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());

        // 交换前后缓冲区。
        //
        // OpenGL 通常是双缓冲：
        // 后缓冲：当前正在画
        // 前缓冲：当前正在显示
        //
        // swap 后，刚画好的画面显示到窗口。
        glfwSwapBuffers(window_);

        // 标记已经渲染过至少一帧。
        has_rendered_frame_ = true;
        return true;
    }

    // 轮询键盘和窗口事件。
    //
    // delay_ms：如果没有按键，最多 sleep 多少毫秒。
    //
    // 返回：
    // 27   -> ESC / 关闭窗口
    // 'q'  -> q 键退出
    // ' '  -> 空格暂停
    // 'f'  -> 切换全屏
    // 's'  -> 截图
    // 'r'  -> 重置统计
    // -1   -> 没有按键
    int PollKey(int delay_ms) override {
        if (window_ == nullptr) {
            return 27;
        }

        // 处理 GLFW 事件。
        //
        // 这一步会刷新键盘、鼠标、窗口关闭等状态
        glfwPollEvents();

        // 如果用户点了窗口关闭按钮。
        if (glfwWindowShouldClose(window_)) {
            return 27;
        }

        // 检查 H 键是否按下。 H 用来显示/隐藏 HUD
        const bool h_key_down = glfwGetKey(window_, GLFW_KEY_H) == GLFW_PRESS;

        // 如果这一帧 H 按下，但上一帧 H 没按下。
        //
        // 这叫“按键边沿触发”。
        // 只在按下瞬间触发一次，避免按住 H 时疯狂切换。
        if (h_key_down && !h_key_was_down_) {
            hud_visible_ = !hud_visible_;
        }

        // 记录本次 H 键状态，供下一次判断
        h_key_was_down_ = h_key_down;

        // 如果 ESC 按下
        if (glfwGetKey(window_, GLFW_KEY_ESCAPE) == GLFW_PRESS) {
            return 27;
        }

        // 如果 Q 按下
        if (glfwGetKey(window_, GLFW_KEY_Q) == GLFW_PRESS) {
            return 'q';
        }

        // 检查空格键
        const bool space_down = glfwGetKey(window_, GLFW_KEY_SPACE) == GLFW_PRESS;

        // 空格键边沿触发
        if (space_down && !space_key_was_down_) {
            space_key_was_down_ = space_down;
            return ' ';
        }

        // 更新空格键状态
        space_key_was_down_ = space_down;

        // 检查 F 键
        const bool f_key_down = glfwGetKey(window_, GLFW_KEY_F) == GLFW_PRESS;

        // F 键边沿触发
        if (f_key_down && !f_key_was_down_) {
            f_key_was_down_ = f_key_down;
            return 'f';
        }

        // 更新 F 键状态。
        f_key_was_down_ = f_key_down;

        // 检查 S 键
        const bool s_key_down = glfwGetKey(window_, GLFW_KEY_S) == GLFW_PRESS;

        // S 键边沿触发
        if (s_key_down && !s_key_was_down_) {
            s_key_was_down_ = s_key_down;
            return 's';
        }

        // 更新 S 键状态
        s_key_was_down_ = s_key_down;

        // 检查 R 键
        const bool r_key_down = glfwGetKey(window_, GLFW_KEY_R) == GLFW_PRESS;

        // R 键边沿触发
        if (r_key_down && !r_key_was_down_) {
            r_key_was_down_ = r_key_down;
            return 'r';
        }

        // 更新 R 键状态
        r_key_was_down_ = r_key_down;

        // 如果没有按键，并且调用者希望等待一小段时间
        if (delay_ms > 0) {
            std::this_thread::sleep_for(std::chrono::milliseconds(delay_ms));
        }
        return -1;
    }

    // 更新窗口标题。
    void UpdateWindowTitle(const std::string &title) override {
        if (window_ != nullptr) {
            // GLFW 设置窗口标题。
            //
            // title.c_str() 把 std::string 转成 C 字符串。
            glfwSetWindowTitle(window_, title.c_str());
        }
    }

    // 切换全屏 / 窗口模式
    void ToggleFullscreen() override {
        if (window_ == nullptr) {
            return;
        }

        // 如果当前已经是全屏
        if (is_fullscreen_) {
            glfwSetWindowMonitor(window_, nullptr, windowed_x_, windowed_y_, windowed_width_, windowed_height_, 0);
            is_fullscreen_ = false;

        // 如果当前不是全屏
        } else {
            // 先记录当前窗口位置，这样退出全屏时可以恢复原来的位置。
            glfwGetWindowPos(window_, &windowed_x_, &windowed_y_);

            // 记录当前窗口大小
            glfwGetWindowSize(window_, &windowed_width_, &windowed_height_);

            // 获取主显示器
            GLFWmonitor *monitor = glfwGetPrimaryMonitor();

            // 如果成功拿到显示器
            if (monitor != nullptr) {
                // 获取显示器当前视频模式，里面有屏幕宽、高、刷新率。
                const GLFWvidmode *mode = glfwGetVideoMode(monitor);

                // 如果视频模式有效
                if (mode != nullptr) {
                    glfwSetWindowMonitor(window_, monitor, 0, 0, mode->width, mode->height, mode->refreshRate);
                    is_fullscreen_ = true;
                }
            }
        }
    }

    // 保存当前窗口画面截图。
    //
    // path：保存路径，比如 screenshot.png。
    // error_message：失败时返回错误信息。
    bool SaveScreenshot(const std::string &path, std::string *error_message) override {
        if (window_ == nullptr || !has_rendered_frame_) {
            if (error_message != nullptr) {
                *error_message = "no frame available for screenshot";
            }
            return false;
        }

        int width = 0;
        int height = 0;

        // 获取当前 framebuffer 尺寸。
        //
        // 注意不是窗口逻辑尺寸，而是真正像素尺寸。
        glfwGetFramebufferSize(window_, &width, &height);
        if (width <= 0 || height <= 0) {
            if (error_message != nullptr) {
                *error_message = "invalid framebuffer size for screenshot";
            }
            return false;
        }

        // 确保当前线程使用这个窗口的 OpenGL context
        glfwMakeContextCurrent(window_);

        // 重置一些 OpenGL 状态，避免 PBO 或 framebuffer 绑定影响截图读取。
        glBindBuffer(GL_PIXEL_UNPACK_BUFFER, 0);
        glBindBuffer(GL_PIXEL_PACK_BUFFER, 0);
        glBindFramebuffer(GL_FRAMEBUFFER, 0);
        glPixelStorei(GL_PACK_ALIGNMENT, 1);

        // 一行 RGB 数据字节数
        const std::size_t row_bytes = static_cast<std::size_t>(width) * 3;

        // 分配截图像素缓冲
        std::vector<std::uint8_t> pixels(row_bytes * static_cast<std::size_t>(height));

        // OpenGL 的坐标原点在左下角。
        // PNG 图片通常希望第一行是顶部。
        //
        // 所以这里按行读取时顺便上下翻转
        for (int y = 0; y < height; ++y) {
            // 从 framebuffer 读取第 y 行像素
            glReadPixels(0, y, width, 1, GL_RGB, GL_UNSIGNED_BYTE,
                         pixels.data() + static_cast<std::size_t>(height - 1 - y) * row_bytes);
        }

        // 使用 stb_image_write 写 PNG
        if (stbi_write_png(path.c_str(), width, height, 3, pixels.data(), static_cast<int>(row_bytes)) == 0) {
            if (error_message != nullptr) {
                *error_message = "failed to write screenshot: " + path;
            }
            return false;
        }

        common::log::Logger::Info(std::string("screenshot saved: ") + path);
        return true;
    }

    // 关闭 OpenGL 渲染后端，释放所有资源
    void Shutdown() override {
        if (window_ != nullptr) {
            // 确保当前线程使用这个窗口对应的 OpenGL context
            glfwMakeContextCurrent(window_);

            // 如果 ImGui 已初始化
            if (imgui_initialized_) {
                // 关闭 ImGui OpenGL3 后端。
                ImGui_ImplOpenGL3_Shutdown();

                // 关闭 ImGui GLFW 后端。
                ImGui_ImplGlfw_Shutdown();

                // 销毁 ImGui context。
                ImGui::DestroyContext();
                imgui_initialized_ = false;
            }

            // 如果任意一个 PBO 存在
            if (pixel_unpack_buffer_ids_[0][0] != 0 ||
                pixel_unpack_buffer_ids_[0][1] != 0 ||
                pixel_unpack_buffer_ids_[1][0] != 0 ||
                pixel_unpack_buffer_ids_[1][1] != 0 ||
                pixel_unpack_buffer_ids_[2][0] != 0 ||
                pixel_unpack_buffer_ids_[2][1] != 0) {
                // 删除 6 个 PBO
                glDeleteBuffers(6, &pixel_unpack_buffer_ids_[0][0]);
            }

            if (shader_program_id_ != 0) {
                glDeleteProgram(shader_program_id_);
            }
            if (element_buffer_id_ != 0) {
                glDeleteBuffers(1, &element_buffer_id_);
            }
            if (vertex_buffer_id_ != 0) {
                glDeleteBuffers(1, &vertex_buffer_id_);
            }
            if (vertex_array_id_ != 0) {
                glDeleteVertexArrays(1, &vertex_array_id_);
            }
            if (texture_ids_[0] != 0 || texture_ids_[1] != 0 || texture_ids_[2] != 0) {
                glDeleteTextures(3, texture_ids_);
            }

            // 销毁 GLFW 窗口
            glfwDestroyWindow(window_);
        }

        // 如果 GLFW 初始化过
        if (glfw_initialized_) {
            glfwTerminate();
        }

        // 把成员变量全部恢复默认状态
        ResetState();
    }

    // 返回当前渲染后端类型
    RenderBackend backend() const override {
        return RenderBackend::kOpenGl;
    }

     // 返回当前渲染后端名字
    const std::string &backend_name() const override {
        return backend_name_;
    }

private:
    // 重置每个纹理平面的缓存尺寸。
    void ResetPlaneDimensions() {
        for (int index = 0; index < 3; ++index) {
            texture_widths_[index] = 0;
            texture_heights_[index] = 0;
        }
    }

    // 根据帧的颜色格式，把 DecodedFrame 的各个 plane 上传到 OpenGL 纹理。
    //
    // frame：解码后的 YUV 帧。
    // color_mode：YUV420P 或 NV12。
    void UploadFrameTextures(const DecodedFrame &frame, NativeColorMode color_mode) {
        if (color_mode == NativeColorMode::kYuv420p) {
            // 上传 Y 平面。
            //
            // YUV420P 的 Y 平面尺寸是：
            // width x height
            UploadPlaneTexture(
                    texture_ids_[0],
                    0,
                    frame.width,
                    frame.height,
                    frame.data[0],
                    frame.linesize[0],
                    GL_R8,
                    GL_RED,
                    pixel_unpack_buffer_ids_[0],
                    pixel_unpack_buffer_sizes_[0],
                    next_pixel_unpack_buffer_index_[0],
                    texture_widths_[0],
                    texture_heights_[0]);

            // 上传 U 平面
            UploadPlaneTexture(
                    texture_ids_[1],
                    1,
                    std::max(1, frame.width / 2),
                    std::max(1, frame.height / 2),
                    frame.data[1],
                    frame.linesize[1],
                    GL_R8,
                    GL_RED,
                    pixel_unpack_buffer_ids_[1],
                    pixel_unpack_buffer_sizes_[1],
                    next_pixel_unpack_buffer_index_[1],
                    texture_widths_[1],
                    texture_heights_[1]);

            // 上传 V 平面
            UploadPlaneTexture(
                    texture_ids_[2],
                    2,
                    std::max(1, frame.width / 2),
                    std::max(1, frame.height / 2),
                    frame.data[2],
                    frame.linesize[2],
                    GL_R8,
                    GL_RED,
                    pixel_unpack_buffer_ids_[2],
                    pixel_unpack_buffer_sizes_[2],
                    next_pixel_unpack_buffer_index_[2],
                    texture_widths_[2],
                    texture_heights_[2]);
            return;
        }

        // 如果不是 YUV420P，这里按 NV12 处理。
        UploadPlaneTexture(
                texture_ids_[0],
                0,
                frame.width,
                frame.height,
                frame.data[0],
                frame.linesize[0],
                GL_R8,
                GL_RED,
                pixel_unpack_buffer_ids_[0],
                pixel_unpack_buffer_sizes_[0],
                next_pixel_unpack_buffer_index_[0],
                texture_widths_[0],
                texture_heights_[0]);
        UploadPlaneTexture(
                texture_ids_[1],
                1,
                std::max(1, frame.width / 2),
                std::max(1, frame.height / 2),
                frame.data[1],
                frame.linesize[1],
                GL_RG8,
                GL_RG,
                pixel_unpack_buffer_ids_[1],
                pixel_unpack_buffer_sizes_[1],
                next_pixel_unpack_buffer_index_[1],
                texture_widths_[1],
                texture_heights_[1]);
    }

    // 把 OpenGL renderer 的所有成员变量恢复成默认状态。
    void ResetState() {
        window_ = nullptr;
        for (int plane = 0; plane < 3; ++plane) {
            texture_ids_[plane] = 0;
            texture_widths_[plane] = 0;
            texture_heights_[plane] = 0;
            next_pixel_unpack_buffer_index_[plane] = 0;
            for (std::size_t slot = 0; slot < kPixelUploadBufferCount; ++slot) {
                pixel_unpack_buffer_ids_[plane][slot] = 0;
                pixel_unpack_buffer_sizes_[plane][slot] = 0;
            }
        }
        vertex_array_id_ = 0;
        vertex_buffer_id_ = 0;
        element_buffer_id_ = 0;
        shader_program_id_ = 0;
        scale_uniform_location_ = -1;
        color_mode_uniform_location_ = -1;
        glfw_initialized_ = false;
        imgui_initialized_ = false;
        window_sized_to_frame_ = false;
        window_title_.clear();
        vsync_enabled_ = false;
        hud_visible_ = true;
        h_key_was_down_ = false;
        space_key_was_down_ = false;
        f_key_was_down_ = false;
        s_key_was_down_ = false;
        r_key_was_down_ = false;
        has_rendered_frame_ = false;
        last_render_timestamp_ns_ = 0;
        displayed_fps_ = 0.0;
        is_fullscreen_ = false;
        windowed_x_ = 0;
        windowed_y_ = 0;
        windowed_width_ = 640;
        windowed_height_ = 480;
    }

    // GLFW 窗口对象。
    GLFWwindow *window_ = nullptr;

    // 3 张 OpenGL 纹理。
    //
    // YUV420P:
    // texture_ids_[0] = Y
    // texture_ids_[1] = U
    // texture_ids_[2] = V
    //
    // NV12:
    // texture_ids_[0] = Y
    // texture_ids_[1] = UV
    GLuint texture_ids_[3] = {0, 0, 0};

    // 每个 plane 两个 PBO，用于双缓冲上传。
    GLuint pixel_unpack_buffer_ids_[3][2] = {{0, 0}, {0, 0}, {0, 0}};

    // 每个 PBO 当前分配的大小。
    std::size_t pixel_unpack_buffer_sizes_[3][2] = {{0, 0}, {0, 0}, {0, 0}};

    // 每个 plane 下次使用哪个 PBO。
    std::size_t next_pixel_unpack_buffer_index_[3] = {0, 0, 0};

    // VAO：记录顶点属性配置。
    GLuint vertex_array_id_ = 0;

    // VBO：保存矩形顶点数据。
    GLuint vertex_buffer_id_ = 0;

    // EBO：保存矩形索引数据。
    GLuint element_buffer_id_ = 0;

    // shader program。
    GLuint shader_program_id_ = 0;

    // u_scale uniform 位置。
    GLint scale_uniform_location_ = -1;

    // u_color_mode uniform 位置。
    GLint color_mode_uniform_location_ = -1;

    // 每张纹理当前缓存的宽度。
    int texture_widths_[3] = {0, 0, 0};

    // 每张纹理当前缓存的高度。
    int texture_heights_[3] = {0, 0, 0};

    // GLFW 是否已初始化。
    bool glfw_initialized_ = false;

    // ImGui 是否已初始化。
    bool imgui_initialized_ = false;

    // 是否已经记录过窗口初始尺寸。
    bool window_sized_to_frame_ = false;

    // 窗口标题。
    std::string window_title_;

    // 后端名称。
    const std::string backend_name_ = "opengl";

    // 是否开启 vsync。
    bool vsync_enabled_ = false;

    // HUD 是否可见。
    bool hud_visible_ = true;

    // 各按键上一帧是否按下，用于边沿触发。
    bool h_key_was_down_ = false;
    bool space_key_was_down_ = false;
    bool f_key_was_down_ = false;
    bool s_key_was_down_ = false;
    bool r_key_was_down_ = false;

    // 是否已经渲染过至少一帧，用于判断能不能截图。
    bool has_rendered_frame_ = false;

    // 上一次渲染时间。
    std::uint64_t last_render_timestamp_ns_ = 0;

    // 平滑后的 FPS。
    double displayed_fps_ = 0.0;

    // 是否全屏。
    bool is_fullscreen_ = false;

    // 窗口模式下的位置和大小，用于退出全屏时恢复。
    int windowed_x_ = 0;
    int windowed_y_ = 0;
    int windowed_width_ = 640;
    int windowed_height_ = 480;
};

}  // namespace

// 创建 OpenGL 渲染后端。
std::unique_ptr<VideoRendererBackend> CreateOpenGlVideoRendererBackend() {
    return std::unique_ptr<VideoRendererBackend>(new OpenGlVideoRendererBackend());
}

}  // namespace sclient
