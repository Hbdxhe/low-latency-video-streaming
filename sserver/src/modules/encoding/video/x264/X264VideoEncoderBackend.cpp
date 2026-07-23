#include "modules/encoding/video/x264/X264VideoEncoderBackend.h"

#include <cstdlib>
#include <cstring>

#ifdef SSERVER_HAS_NEON
#include <arm_neon.h>
#endif

#include "common/log/Logger.h"

namespace sserver {
namespace modules {
namespace encoding {

namespace {

#ifdef SSERVER_HAS_NEON
// ARM64 / NEON 优化版本：将 YUYV422 转换为 I420/YUV420P。
//
// 输入格式：YUYV422
// 每两个像素占 4 字节，布局如下：
//      Y0 U0 Y1 V0
//
// 输出格式：I420 / YUV420P
// 三个平面分别存储：
//      Y 平面：width * height
//      U 平面：width/2 * height/2
//      V 平面：width/2 * height/2
//
// 转换目的：
// x264 通常使用 I420 作为输入格式，因此摄像头采集到的 YUYV422
// 需要先转换为 I420，再送入 x264 编码。
void ConvertYuyv422ToI420Neon(
        const std::uint8_t *src,
        std::uint8_t *y_plane,
        std::uint8_t *u_plane,
        std::uint8_t *v_plane,
        int width,
        int height) {
    // YUYV422 每个像素平均 2 字节，因此一行字节数为 width * 2
    const int row_bytes = width * 2;

    // NEON 每次处理 16 个像素，16 个像素的 YUYV422 数据占 32 字节
    const int neon_pixels = 16;

    // 对齐到 16 像素边界，不能整除 16 的尾部像素后面用标量方式处理。
    const int aligned_width = (width / neon_pixels) * neon_pixels;

    // I420 的 U/V 平面宽度是 Y 平面的一半
    const int uv_row_stride = width / 2;

    // I420 的 U/V 垂直方向也下采样 2 倍。
    // 因此每次处理两行：偶数行提取 Y/U/V，奇数行只提取 Y。
    for (int row = 0; row < height; row += 2) {
        // 当前偶数行的 YUYV422 输入地址
        const std::uint8_t *src_even = src + static_cast<std::size_t>(row) * row_bytes;

        // 当前偶数行在 Y 平面中的输出地址
        std::uint8_t *y_row_even = y_plane + static_cast<std::size_t>(row) * width;

        // 当前 row/2 对应的 U/V 平面输出行
        std::uint8_t *u_row = u_plane + static_cast<std::size_t>(row / 2) * uv_row_stride;
        std::uint8_t *v_row = v_plane + static_cast<std::size_t>(row / 2) * uv_row_stride;

        // 处理当前偶数行中可以被 NEON 对齐处理的部分。
        for (int col = 0; col < aligned_width; col += neon_pixels) {
            const std::uint8_t *p = src_even + col * 2;

            // vld2q_u8 会从交错数据中拆出偶数字节和奇数字节：
            //
            // 原始 YUYV422：
            //      Y0 U0 Y1 V0 Y2 U1 Y3 V1 ...
            //
            // yuyv.val[0] 得到所有 Y：
            //      Y0 Y1 Y2 Y3 ...
            //
            // yuyv.val[1] 得到交错的 U/V：
            //      U0 V0 U1 V1 ...
            uint8x16x2_t yuyv = vld2q_u8(p);

            // 写入 16 个像素的 Y 值
            vst1q_u8(y_row_even + col, yuyv.val[0]);

            // yuyv.val[1] 中仍然是 U/V 交错排列：
            //      U0 V0 U1 V1 ...
            //
            // 这里需要继续把 U 和 V 拆分成两个独立平面
            uint8x8_t u_lo = vget_low_u8(yuyv.val[1]);
            uint8x8_t v_lo = vget_high_u8(yuyv.val[1]);

            // vuzp_u8 用于解交错：
            //      uzp.val[0] 得到 U0 U1 U2 ...
            //      uzp.val[1] 得到 V0 V1 V2 ...
            uint8x8x2_t uzp = vuzp_u8(u_lo, v_lo);

            // 写入 I420 的 U/V 平面
            vst1_u8(u_row + col / 2, uzp.val[0]);
            vst1_u8(v_row + col / 2, uzp.val[1]);
        }

        // 处理不能被 16 像素整除的尾部部分
        for (int col = aligned_width; col < width; col += 2) {
            const std::uint8_t *p = src_even + col * 2;

            // YUYV422 两个像素布局：
            //      p[0] = Y0
            //      p[1] = U
            //      p[2] = Y1
            //      p[3] = V
            y_row_even[col] = p[0];
            y_row_even[col + 1] = p[2];

            // I420 的 U/V 水平方向下采样 2 倍，
            // 因此 col 和 col+1 两个像素共用一个 U/V。
            u_row[col / 2] = p[1];
            v_row[col / 2] = p[3];
        }

        // 处理奇数行。
        //
        // 对 I420 来说，U/V 在垂直方向下采样 2 倍。
        // 当前实现直接使用偶数行的 U/V 作为这一对行的色度值，
        // 所以奇数行只需要提取 Y 平面，不再写 U/V。
        if (row + 1 < height) {
            const std::uint8_t *src_odd = src + static_cast<std::size_t>(row + 1) * row_bytes;
            std::uint8_t *y_row_odd = y_plane + static_cast<std::size_t>(row + 1) * width;

            // NEON 提取奇数行 Y 值。
            for (int col = 0; col < aligned_width; col += neon_pixels) {
                uint8x16x2_t yuyv = vld2q_u8(src_odd + col * 2);
                vst1q_u8(y_row_odd + col, yuyv.val[0]);
            }

            // 处理尾部像素
            for (int col = aligned_width; col < width; col += 2) {
                const std::uint8_t *p = src_odd + col * 2;
                y_row_odd[col] = p[0];
                y_row_odd[col + 1] = p[2];
            }
        }
    }
}
#endif

// 通用 C++ 标量版本：将 YUYV422 转换为 I420/YUV420P。
//
// 该版本不依赖 SIMD 指令，所有平台都可以使用。
// x86_64 当前会走这条路径；ARM64 如果定义了 SSERVER_HAS_NEON，
// 则优先使用上面的 NEON 优化版本。
void ConvertYuyv422ToI420Scalar(
        const std::uint8_t *src,
        std::uint8_t *y_plane,
        std::uint8_t *u_plane,
        std::uint8_t *v_plane,
        int width,
        int height) {
    // YUYV422 每个像素平均 2 字节。
    // 因此一行原始数据长度为 width * 2。
    const std::size_t row_bytes = static_cast<std::size_t>(width) * 2;

    // 第一步：提取所有亮度 Y 值。
    //
    // YUYV422 两个像素布局：
    //      Y0 U Y1 V
    //
    // 所以每次 col += 2，取出两个像素的 Y：
    //      src[row_start + col * 2]     -> Y0
    //      src[row_start + col * 2 + 2] -> Y1
    int y_index = 0;
    for (int row = 0; row < height; ++row) {
        const std::size_t row_start = static_cast<std::size_t>(row) * row_bytes;
        for (int col = 0; col < width; col += 2) {
            y_plane[y_index++] = src[row_start + col * 2];
            y_plane[y_index++] = src[row_start + col * 2 + 2];
        }
    }

    // 第二步：提取 U/V 色度分量。
    //
    // YUYV422 是 4:2:2：
    //      水平方向两个像素共享一组 U/V；
    //      垂直方向每一行都有 U/V。
    //
    // I420 是 4:2:0：
    //      水平方向两个像素共享一组 U/V；
    //      垂直方向两行共享一组 U/V。
    //
    // 当前实现从偶数行提取 U/V，作为两行共享的色度值。
    // 这是简单快速的 4:2:2 -> 4:2:0 下采样方式。
    for (int row = 0; row < height; row += 2) {
        const std::size_t row_start = static_cast<std::size_t>(row) * row_bytes;

        // 当前要写入 I420 的 U/V 平面中的哪一行的起始下标
        const int uv_row = (row / 2) * (width / 2);
        for (int col = 0; col < width; col += 2) {
            const std::size_t offset = row_start + col * 2;

            // YUYV422 当前两个像素：
            //      offset + 0 : Y0
            //      offset + 1 : U
            //      offset + 2 : Y1
            //      offset + 3 : V
            u_plane[uv_row + col / 2] = src[offset + 1];
            v_plane[uv_row + col / 2] = src[offset + 3];
        }
    }
}

}  // namespace

std::unique_ptr<VideoEncoderBackend> CreateX264VideoEncoderBackend() {
    // 工厂函数，供 VideoEncoderBackendFactory 创建 x264 后端。
    return std::unique_ptr<VideoEncoderBackend>(new X264VideoEncoderBackend());
}

X264VideoEncoderBackend::X264VideoEncoderBackend()
        : backend_name_("x264"),
          param_(nullptr),
          handle_(nullptr),
          picture_(nullptr),
          nal_(nullptr),
          pts_(0) {
}

X264VideoEncoderBackend::~X264VideoEncoderBackend() {
    Shutdown();
}

bool X264VideoEncoderBackend::Initialize(
        int width,
        int height,
        int fps,
        const config::CodecConfig &config,
        std::string *error_message) {
    // 重新初始化前先释放旧资源，保证 Initialize 可重复调用。
    Shutdown();

    // x264 使用 C API，需要手动分配参数结构和输入 picture。
    param_ = static_cast<x264_param_t *>(std::malloc(sizeof(x264_param_t)));
    picture_ = static_cast<x264_picture_t *>(std::malloc(sizeof(x264_picture_t)));
    if (param_ == nullptr || picture_ == nullptr) {
        common::log::Logger::Error("x264 allocation failed");
        if (error_message != nullptr) {
            *error_message = "x264 allocation failed";
        }
        Shutdown();
        return false;
    }

    // 先加载 x264 默认参数，再应用 preset/tune。
    // ultrafast + zerolatency 是低延迟实时视频常见组合。
    x264_param_default(param_);
    x264_param_default_preset(param_, config.x264_preset.c_str(), config.x264_tune.c_str());
    // 基础视频参数。
    param_->i_width = width;
    param_->i_height = height;
    // lookahead 相关参数设为 0 可以减少编码器等待未来帧带来的延迟。
    param_->rc.i_lookahead = config.x264_lookahead;
    param_->i_sync_lookahead = config.x264_sync_lookahead;
    // 帧率参数。
    param_->i_fps_num = fps;
    param_->i_fps_den = 1;
    // Annex-B 输出会带 start code，便于 RTP/UDP/TCP 后续按 NALU 处理。
    param_->b_annexb = config.x264_annexb ? 1 : 0;
    // GOP/关键帧间隔。关键帧更频繁，客户端恢复更快，但码率更高。
    param_->i_keyint_max = config.x264_keyint_max;
    param_->i_keyint_min = config.x264_keyint_min;
    // B 帧会引入重排序延迟，低延迟场景通常设为 0。
    param_->i_bframe = config.x264_bframes;
    // 重复 SPS/PPS，客户端中途加入时更容易获得解码参数。
    param_->b_repeat_headers = config.x264_repeat_headers ? 1 : 0;
    // 线程和 slice 设置。低延迟场景更关注排队少、输出快。
    param_->i_threads = config.x264_threads;
    param_->b_sliced_threads = config.x264_sliced_threads ? 1 : 0;
    param_->i_slice_count = config.x264_slice_count;
    param_->i_slice_count_max = config.x264_slice_count_max;
    param_->i_frame_reference = config.x264_frame_reference;
    param_->analyse.i_subpel_refine = config.x264_subpel_refine;
    param_->rc.b_mb_tree = config.x264_mb_tree ? 1 : 0;
    param_->i_scenecut_threshold = config.x264_scenecut;

    // 应用 H.264 profile，例如 baseline。
    x264_param_apply_profile(param_, config.x264_profile.c_str());

    // 打开 x264 编码器实例。
    handle_ = x264_encoder_open(param_);
    if (handle_ == nullptr) {
        common::log::Logger::Error("x264_encoder_open failed");
        if (error_message != nullptr) {
            *error_message = "x264_encoder_open failed";
        }
        Shutdown();
        return false;
    }

    // x264 输入 picture 使用 I420/YUV420P。
    x264_picture_alloc(picture_, X264_CSP_I420, width, height);
    picture_->img.i_csp = X264_CSP_I420;
    picture_->img.i_plane = 3;
    pts_ = 0;
    return true;
}

// 编码一帧 YUYV422 原始图像为 H264 码流
// 流程：色彩空间转换 (YUYV→I420) → x264 编码 → 收集 NAL 输出
bool X264VideoEncoderBackend::EncodeYuyv422Frame(
        const std::uint8_t *input,
        std::size_t input_length,
        std::vector<std::uint8_t> *output,
        bool *is_keyframe,
        std::string *error_message) {

    // 检查输入输出参数和内部编码器状态。
    //
    // handle_ 为空说明 x264_encoder_open 尚未成功；
    // picture_ 为空说明输入图像缓冲区未分配；
    // param_ 为空说明编码参数未初始化。
    if (input == nullptr || output == nullptr || is_keyframe == nullptr ||
        handle_ == nullptr || picture_ == nullptr || param_ == nullptr) {
        if (error_message != nullptr) {
            *error_message = "x264 encoder is not initialized";
        }
        return false;
    }

    const int width = param_->i_width;
    const int height = param_->i_height;
    // YUYV422 每个像素平均 2 字节，所以期望长度是 width * height * 2。
    const std::size_t expected_length = static_cast<std::size_t>(width * height * 2);
    if (input_length < expected_length) {
        common::log::Logger::Warn("input frame is shorter than expected YUYV422 buffer");
        if (error_message != nullptr) {
            *error_message = "input frame is shorter than expected YUYV422 buffer";
        }
        return false;
    }

#ifdef SSERVER_HAS_NEON
    // ARM64 上使用 NEON 加速颜色格式转换。
    // 转换结果直接写入 x264_picture_t 的三个平面：
    //      plane[0] = Y
    //      plane[1] = U
    //      plane[2] = V
    ConvertYuyv422ToI420Neon(
            input,
            reinterpret_cast<std::uint8_t *>(picture_->img.plane[0]),
            reinterpret_cast<std::uint8_t *>(picture_->img.plane[1]),
            reinterpret_cast<std::uint8_t *>(picture_->img.plane[2]),
            width,
            height);
#else
    // 其他平台使用通用标量转换。
    ConvertYuyv422ToI420Scalar(
            input,
            reinterpret_cast<std::uint8_t *>(picture_->img.plane[0]),
            reinterpret_cast<std::uint8_t *>(picture_->img.plane[1]),
            reinterpret_cast<std::uint8_t *>(picture_->img.plane[2]),
            width,
            height);
#endif

    // 让 x264 自动决定帧类型；关键帧由 keyint 配置控制。
    picture_->i_type = X264_TYPE_AUTO;
    // PTS 单调递增，供 x264 维护编码顺序。
    picture_->i_pts = pts_++;

    // x264 编码后的输出图像信息。可以通过 picture_out.b_keyframe 判断是否为关键帧。
    x264_picture_t picture_out{};

    // x264_encoder_encode 输出的 NALU 数量。
    int nal_count = 0;

    // 清空上一帧可能残留的输出
    output->clear();

    // 默认不是关键帧，编码成功后根据 picture_out 更新
    *is_keyframe = false;

    // 调用 x264 编码。输出是若干 NAL 单元。
    // 参数说明：
    // 1. handle_
    //      x264 编码器句柄；
    //
    // 2. &nal_
    //      输出 NALU 数组指针, i_payload 是这个 NALU 的数据长度, p_payload 是当前 NALU 数据的起始地址
    //
    // 3. &nal_count
    //      输出 NALU 个数；
    //
    // 4. picture_
    //      输入图像；
    //
    // 5. &picture_out
    //      输出图像信息。
    //
    // 返回值小于 0 表示编码失败。
    if (x264_encoder_encode(handle_, &nal_, &nal_count, picture_, &picture_out) < 0) {
        common::log::Logger::Error("x264_encoder_encode failed");
        if (error_message != nullptr) {
            *error_message = "x264_encoder_encode failed";
        }
        return false;
    }
    *is_keyframe = picture_out.b_keyframe != 0;

    // 先统计总大小，再一次 reserve，减少 vector 扩容。
    std::size_t total_size = 0;
    for (int index = 0; index < nal_count; ++index) {
        total_size += static_cast<std::size_t>(nal_[index].i_payload);
    }
    output->reserve(total_size);

    // 将本帧输出的所有 NALU payload 顺序拼接到 output。
    //
    // x264 可能输出多个 NALU，例如：
    //      SPS
    //      PPS
    //      SEI
    //      IDR slice
    //      P slice
    //
    // 如果开启 Annex-B，每个 NALU payload 中通常已经带 start code。
    for (int index = 0; index < nal_count; ++index) {
        const std::uint8_t *payload = nal_[index].p_payload;
        output->insert(output->end(), payload, payload + nal_[index].i_payload);
    }

    // 某些情况下，x264 本次调用可能没有输出码流。
    //
    // 例如编码器内部缓存、B 帧重排序等场景可能导致延迟输出。
    // 但低延迟配置下一般期望每帧都有输出。
    if (output->empty() && error_message != nullptr) {
        *error_message = "x264 encoder did not produce output";
    }
    return !output->empty();
}

void X264VideoEncoderBackend::Shutdown() {
    // 按资源类型逐一释放 x264 对象。

    // x264_picture_alloc() 分配的图像内部缓冲区，需要通过 x264_picture_clean() 清理。
    if (picture_ != nullptr) {
        x264_picture_clean(picture_);
        std::free(picture_);
        picture_ = nullptr;
    }

    // 释放 x264 参数结构
    if (param_ != nullptr) {
        std::free(param_);
        param_ = nullptr;
    }

    // 关闭 x264 编码器。
    // handle_ 是 x264_encoder_open() 创建的编码器句柄，需要通过 x264_encoder_close() 释放。
    if (handle_ != nullptr) {
        x264_encoder_close(handle_);
        handle_ = nullptr;
    }

    // nal_ 指向 x264 内部输出的 NALU 数组。
    // 该指针不由本类手动 free，只需要置空，避免误用。
    nal_ = nullptr;

    // 重置 PTS
    pts_ = 0;
}

EncodeBackend X264VideoEncoderBackend::backend() const {
    // 返回当前后端类型
    return EncodeBackend::kX264;
}

const std::string &X264VideoEncoderBackend::backend_name() const {
     // 返回当前后端名称，用于日志或状态展示
    return backend_name_;
}

}  // namespace encoding
}  // namespace modules
}  // namespace sserver
