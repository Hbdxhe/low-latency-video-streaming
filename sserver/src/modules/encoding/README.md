# modules/encoding

## 1. 模块定位

视频编码模块。提供 YUYV422 原始帧到 H.264 Annex-B 的编码能力，采用「门面 + backend 接口 + 工厂」模式，当前实现 x264 软件编码后端。

## 2. 核心职责

- 提供 `VideoEncoderBackend` 抽象接口，定义编码器后端能力
- 提供 `VideoEncoder` 门面类，封装后端创建和生命周期管理
- 实现 `X264VideoEncoderBackend`：基于 libx264 的软件 H.264 编码
- 支持 ARM64 NEON 加速的 YUYV422 → I420 色彩空间转换
- 支持 x86_64 标量实现的 YUYV422 → I420 色彩空间转换

## 3. 主要文件说明

| 文件 | 作用 |
|---|---|
| video/VideoEncoderBackend.h | 定义 `VideoEncoderBackend` 纯虚接口：`Initialize()`、`EncodeYuyv422Frame()`、`Shutdown()` |
| video/VideoEncoder.h / .cpp | `VideoEncoder` 门面类，封装后端创建（`VideoEncoderBackendFactory`）和生命周期管理 |
| video/x264/X264VideoEncoderBackend.h / .cpp | x264 软件编码后端实现，包含 NEON/scalar 两种 YUYV→I420 转换路径 |

## 4. 核心类 / 函数说明

### VideoEncoderBackend

作用：
- 编码器后端的抽象接口
- 当前仅有一个实现 `X264VideoEncoderBackend`

关键函数：
- `Initialize(width, height, fps, config, error_message)`：初始化编码器
- `EncodeYuyv422Frame(input, input_length, output, is_keyframe, error_message)`：编码一帧 YUYV422 数据为 H.264
- `Shutdown()`：释放编码器资源
- `backend()` / `backend_name()`：返回后端类型

### VideoEncoder

作用：
- 门面类，通过 `VideoEncoderBackendFactory` 创建具体的编码器后端
- 被 `V4L2CaptureDevice` 和 `NullCaptureDevice`（h264_test_pattern 模式）使用

关键函数：
- `Initialize(width, height, fps, config, error_message)`：解析编码后端配置，创建后端实例并初始化
- `EncodeYuyv422Frame(input, input_length, output, is_keyframe, error_message)`：代理到后端实现
- `Shutdown()`：释放后端资源

### X264VideoEncoderBackend

作用：
- 基于 libx264 的 H.264 软件编码实现
- 使用 `x264_param_default_preset()` 应用编码预设（如 ultrafast/zerolatency）
- 输出 Annex-B 格式的 H.264 数据

关键函数：
- `Initialize()`：
  1. 分配 `x264_param_t` 和 `x264_picture_t`
  2. 应用预设和调优参数
  3. 配置分辨率、帧率、GOP、B 帧、线程等参数
  4. 应用 profile（baseline）
  5. 打开编码器 `x264_encoder_open()`
  6. 分配 I420 图像缓冲区
- `EncodeYuyv422Frame()`：
  1. YUYV422 → I420 色彩空间转换（NEON 或 scalar）
  2. `x264_encoder_encode()` 编码
  3. 收集所有 NAL 单元拼接到输出
- `Shutdown()`：清理 `x264_picture_clean()`、`x264_encoder_close()`、释放内存

### YUYV422 → I420 转换

- **NEON 路径**（ARM64）：`ConvertYuyv422ToI420Neon()` — 使用 `vld2q_u8`/`vst1q_u8` 等 NEON 指令，每次处理 16 像素
- **Scalar 路径**（x86_64）：`ConvertYuyv422ToI420Scalar()` — 标量循环实现

## 5. 数据流说明

输入：
- YUYV422 格式的原始帧数据（来自 V4L2 或 Null 设备）
- 帧大小 = width × height × 2 字节

处理：
1. YUYV422 → I420 色彩空间转换（分离 Y、U、V 平面）
2. x264 编码（应用预设、profile、GOP 等参数）
3. 收集 NAL 单元

输出：
- H.264 Annex-B 格式的编码数据（`vector<uint8_t>`）
- 是否为关键帧（`is_keyframe`）

## 6. 与其他模块的关系

- 被 `V4L2CaptureDevice` 创建和使用（在 `Start()` 中初始化）
- 被 `NullCaptureDevice` 创建和使用（`h264_test_pattern` 模式下）
- 通过 `VideoEncoderBackendFactory` 创建后端实例
- 编码输出填充到 `EncodedFrame::payload`
- 从 `config/AppConfig` 读取 `CodecConfig`

注意：当前编码模块不是独立的 `IModule` 实现，而是作为领域组件被采集模块内部使用。`VideoEncoder` 没有独立的生命周期管理，由持有它的设备实现负责创建和销毁。

## 7. 线程模型

编码模块本身不管理线程。`VideoEncoder` 和 `X264VideoEncoderBackend` 的所有方法都是同步调用。

在双线程采集架构下，编码在 `EncodePump` 线程中执行（由 `CaptureModule` 管理）。

x264 编码器本身是线程不安全的，不应在多线程中并发调用同一个实例。

## 8. 配置参数

| 参数 | 说明 | 默认值 |
|---|---|---|
| codec.backend | 编码后端 | "x264" |
| codec.x264_preset | x264 编码预设 | "ultrafast" |
| codec.x264_tune | x264 调优 | "zerolatency" |
| codec.x264_profile | x264 profile | "baseline" |
| codec.x264_annexb | 输出 Annex-B 格式 | true |
| codec.x264_repeat_headers | 重复 SPS/PPS | true |
| codec.x264_keyint_max | 最大 GOP 大小 | 30 |
| codec.x264_keyint_min | 最小 GOP 大小 | 30 |
| codec.x264_bframes | B 帧数量 | 0 |
| codec.x264_threads | 编码线程数 | 1 |
| codec.x264_sliced_threads | 切片线程模式 | true |
| codec.x264_slice_count | 切片数量 | 1 |
| codec.x264_frame_reference | 参考帧数 | 1 |
| codec.x264_lookahead | lookahead 帧数 | 0 |
| codec.x264_subpel_refine | 亚像素运动估计精度 | 0 |
| codec.x264_mb_tree | MB-tree 码率控制 | false |
| codec.x264_scenecut | 场景切换检测阈值 | 0 |

所有参数都围绕低延迟场景配置：`ultrafast` 预设、`zerolatency` 调优、`baseline` profile、无 B 帧、无 lookahead、单线程。

## 9. 调试建议

- 编码失败：检查 "x264_encoder_open failed" 或 "x264_encoder_encode failed" 日志
- 输入帧大小不匹配：检查 "input frame is shorter than expected YUYV422 buffer" 日志
- 编码器未输出数据：检查 "x264 encoder did not produce output" 日志
- 可以在 `X264VideoEncoderBackend::EncodeYuyv422Frame()` 中打断点观察编码过程
- 使用 `capture.null_payload_mode=h264_test_pattern` 可在无摄像头环境下测试编码链路

## 10. 后续扩展方向

- 增加硬件编码后端（VAAPI、NVENC、V4L2 M2M）
- 增加更多软件编码后端（如 OpenH264）
- 将编码模块独立为 `IModule`，支持独立的生命周期管理
- 增加编码质量/码率统计
- 增加自适应码率控制
- 增加音频编码支持（`encoding/audio/`）
