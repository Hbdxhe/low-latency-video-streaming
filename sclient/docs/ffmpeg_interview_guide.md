# FFmpeg 基础知识与项目面试指南

> 适用场景：C++、Linux、音视频、流媒体、客户端开发岗位面试
> 项目背景：H.264 Annex-B 网络码流 -> FFmpeg 软件解码 -> YUV 图像 -> OpenGL 渲染

---

## 1. 面试时先用一句话解释 FFmpeg

FFmpeg 是一套开源的音视频处理工具和开发库，能够完成封装与解封装、编码与解码、像素格式转换、音频重采样、滤镜处理和媒体推流等工作。

需要注意：**FFmpeg 不只是一个命令行程序，也是一组可以被 C/C++ 调用的底层库。**

例如：

```bash
# 使用 ffmpeg 命令把 MP4 文件转成裸 H.264 Annex-B 码流
ffmpeg -i input.mp4 -c:v libx264 -f h264 output.h264
```

在 C++ 项目中，则可以直接调用 `libavcodec`：

```cpp
const AVCodec *codec = avcodec_find_decoder(AV_CODEC_ID_H264);
```

本项目没有启动外部 `ffmpeg` 命令来解码，而是把 FFmpeg 的库链接进客户端进程，通过 C API 完成 H.264 软件解码。

---

## 2. FFmpeg 由哪些核心库组成

| 库 | 主要作用 | 本项目是否直接使用 |
|---|---|---|
| `libavcodec` | 音视频编码与解码 | 是，解码 H.264 |
| `libavformat` | MP4、FLV、MKV、TS 等封装格式的读取和写入 | 否 |
| `libavutil` | 错误处理、像素格式、内存、时间基等公共工具 | 是 |
| `libswscale` | 图像缩放和像素格式转换 | 当前解码链路未使用 |
| `libswresample` | 音频重采样、声道布局和采样格式转换 | 否，本项目没有音频 |
| `libavfilter` | 音视频滤镜，例如裁剪、缩放、水印 | 否 |
| `libavdevice` | 摄像头、麦克风等设备输入输出 | 否，服务端使用 V4L2 直接采集 |

### 面试回答

> 我这个项目在客户端主要使用 `libavcodec` 和 `libavutil`。网络模块已经把 TCP、UDP 或 RTP 数据还原成完整的 H.264 Annex-B 数据，所以不需要 `libavformat` 解封装；我直接把压缩码流送入 H.264 解码器，得到 YUV420P 或 NV12 图像，再交给 OpenGL 渲染。

---

## 3. 必须分清的三个概念

### 3.1 编码格式

编码格式描述如何压缩音视频数据，例如：

- H.264/AVC
- H.265/HEVC
- AV1
- AAC
- Opus

本项目使用 x264 把原始 YUV 图像编码成 H.264，客户端再使用 FFmpeg 解码 H.264。

### 3.2 封装格式

封装格式是一个容器，可以同时存放视频、音频、字幕和时间戳等信息，例如：

- MP4
- FLV
- MKV
- MPEG-TS

MP4 和 H.264 不是同一层概念：

```text
MP4：容器
H.264：容器中可能保存的视频编码格式
AAC：容器中可能保存的音频编码格式
```

### 3.3 裸码流

裸码流只有编码后的媒体数据，没有 MP4、FLV 等容器结构。

本项目网络模块向解码模块交付的是 H.264 Annex-B 裸码流，其 NALU 前通常带有起始码：

```text
00 00 00 01 67 ...   SPS
00 00 00 01 68 ...   PPS
00 00 00 01 65 ...   IDR
```

因此本项目不需要先用 `libavformat` 做解封装。

---

## 4. 本项目中的 FFmpeg 位置

```text
服务端
摄像头 -> YUYV -> I420 -> x264 -> H.264 Annex-B
                                      |
                              TCP / UDP / RTP
                                      |
客户端                                v
网络收包 -> 分片重组 -> 完整 H.264 数据 -> FFmpeg 解码
                                             |
                                             v
                                      YUV420P / NV12
                                             |
                                             v
                                      OpenGL 纹理渲染
```

FFmpeg 在这里负责的是：

```text
压缩的 H.264 数据 -> 解码后的 YUV 图像
```

FFmpeg 不负责本项目中的：

- 摄像头采集；
- TCP、UDP、RTP 收包；
- UDP 分片重组、NACK、FEC；
- RTP FU-A 重组；
- OpenGL 窗口显示。

把模块边界说清楚，是面试中很重要的一点。

---

## 5. H.264 解码前需要知道什么

### 5.1 NALU

NALU 是 H.264 码流的基本数据单元。常见类型如下：

| NALU | 作用 |
|---|---|
| SPS | 描述分辨率、Profile、Level 等序列参数 |
| PPS | 描述熵编码、参考图像等图像参数 |
| IDR | 可独立开始解码的关键图像 |
| 非 IDR Slice | 通常依赖前面的参考图像 |
| SEI | 补充增强信息，可携带额外元数据 |

### 5.2 为什么客户端中途加入可能解不出画面

假设客户端刚连接时收到的是普通 P 帧：

```text
P 帧 -> P 帧 -> P 帧 -> SPS -> PPS -> IDR
```

前面的 P 帧依赖历史参考帧，而且解码器还没有 SPS/PPS，因此 FFmpeg 可能输出：

```text
non-existing PPS referenced
decode_slice_header error
no frame!
```

客户端通常需要等到新的 SPS、PPS 和 IDR 到达，才能从一个完整的随机访问点恢复解码。

可采用的工程策略包括：

- 编码器在关键帧前重复发送 SPS/PPS；
- 新客户端加入时请求服务端尽快产生关键帧；
- 发现码流损坏后丢弃普通帧，等待下一个 IDR；
- 重连或切换码流时调用 `avcodec_flush_buffers()` 清理旧参考状态。

---

## 6. FFmpeg 解码的五个核心对象

### 6.1 AVCodec

`AVCodec` 描述一个具体编解码器实现，可以理解为“解码器类型说明书”。

```cpp
const AVCodec *codec = avcodec_find_decoder(AV_CODEC_ID_H264);
```

这里查找的是 FFmpeg 已注册的 H.264 解码器。

它本身不保存当前视频流的解码状态，通常可以视为只读描述对象，不由业务代码释放。

### 6.2 AVCodecContext

`AVCodecContext` 保存某一路视频流的实际解码状态，例如：

- 编解码器参数；
- 参考帧；
- 内部缓冲区；
- 线程配置；
- 当前解码进度。

```cpp
AVCodecContext *context = avcodec_alloc_context3(codec);
avcodec_open2(context, codec, nullptr);
```

可以把它理解为“真正工作的 H.264 解码器实例”。

一条独立视频流通常对应一个独立的 `AVCodecContext`，不能让多条流随意共用同一个上下文。

### 6.3 AVPacket

`AVPacket` 表示压缩后的音视频数据。

在本项目中，它装的是网络模块交付的 H.264 字节：

```cpp
AVPacket packet{};
packet.data = const_cast<std::uint8_t *>(data);
packet.size = static_cast<int>(size);
```

需要特别注意：

```text
AVPacket：压缩数据，例如 H.264
AVFrame：解码后的原始图像，例如 YUV420P
```

### 6.4 AVFrame

`AVFrame` 保存解码后的音视频帧。对于视频，它包含：

- `width`、`height`：图像宽高；
- `format`：像素格式；
- `data[]`：各图像平面的首地址；
- `linesize[]`：各平面一行实际占用的字节数；
- 时间戳、色彩信息和引用计数缓冲区等。

### 6.5 AVPixelFormat

`AVPixelFormat` 表示解码输出的像素格式，例如：

- `AV_PIX_FMT_YUV420P`
- `AV_PIX_FMT_YUVJ420P`
- `AV_PIX_FMT_NV12`
- `AV_PIX_FMT_RGB24`

本项目把 FFmpeg 的像素格式转换为项目内部枚举，再让 OpenGL 后端选择对应的纹理上传方式。

---

## 7. 初始化解码器的完整流程

```cpp
// 1. 查找 H.264 解码器
const AVCodec *codec = avcodec_find_decoder(AV_CODEC_ID_H264);

// 2. 为这一条视频流创建解码上下文
AVCodecContext *codec_context = avcodec_alloc_context3(codec);

// 3. 打开解码器
int result = avcodec_open2(codec_context, codec, nullptr);

// 4. 创建一个用于接收解码结果的 AVFrame
AVFrame *frame = av_frame_alloc();
```

对应的释放顺序为：

```cpp
av_frame_free(&frame);
avcodec_free_context(&codec_context);
```

FFmpeg 的 `free` 函数通常接收二级指针，释放后会把原指针设置为 `nullptr`，降低重复释放的风险。

### 为什么头文件放在 extern "C" 中

```cpp
extern "C" {
#include <libavcodec/avcodec.h>
#include <libavutil/error.h>
}
```

FFmpeg 的接口主要是 C API。C++ 编译器会对函数名进行名称修饰，而 C 不会。`extern "C"` 告诉 C++ 编译器按照 C 的链接规则查找这些函数，否则可能在链接阶段出现未定义引用。

---

## 8. 现代 FFmpeg 解码 API

现代 FFmpeg 推荐使用：

```cpp
avcodec_send_packet()
avcodec_receive_frame()
```

基本模式如下：

```cpp
int ret = avcodec_send_packet(codec_context, &packet);
if (ret < 0) {
    // 输入压缩数据失败
}

while (true) {
    ret = avcodec_receive_frame(codec_context, frame);

    if (ret == AVERROR(EAGAIN)) {
        // 当前没有更多输出，需要继续输入 packet
        break;
    }

    if (ret == AVERROR_EOF) {
        // 解码器已经结束
        break;
    }

    if (ret < 0) {
        // 真正的解码错误
        break;
    }

    // 成功获得一帧解码后的图像
    ProcessFrame(frame);
    av_frame_unref(frame);
}
```

### 为什么 send 一次后要 receive 多次

输入和输出不是严格的一对一关系：

- 输入一个 packet，可能暂时没有 frame 输出；
- 输入一个 packet，可能输出一帧；
- 输入一个 packet，也可能输出多帧；
- 解码器内部可能因为参考关系而暂存数据。

所以正确模型是：

```text
不断送入压缩数据
每次送入后尽可能取空当前所有输出
```

不能假设“一次 `send_packet` 必然对应一次 `receive_frame`”。

---

## 9. EAGAIN 和 EOF 到底是什么意思

### AVERROR(EAGAIN)

在 `avcodec_receive_frame()` 中，`EAGAIN` 通常表示：

> 当前能输出的帧已经取完了，请继续送入新的压缩数据。

它不代表网络错误，也不代表解码失败，是正常的流式解码状态。

在 `avcodec_send_packet()` 中出现 `EAGAIN`，则表示：

> 解码器内部还有尚未取出的输出，应先调用 `receive_frame()`。

### AVERROR_EOF

表示解码器已经完成排空，后续不会再输出帧。

文件或有限流结束时，通常需要发送空 packet 进入排空状态：

```cpp
avcodec_send_packet(codec_context, nullptr);

while (avcodec_receive_frame(codec_context, frame) == 0) {
    ProcessFrame(frame);
    av_frame_unref(frame);
}
```

实时摄像头流通常持续运行，没有正常文件结尾；只有退出或主动结束流时才需要考虑排空。

---

## 10. AVFrame 的生命周期与零拷贝思想

### 10.1 为什么不能直接保存 data 指针

下面的做法有风险：

```cpp
const uint8_t *y = frame->data[0];
av_frame_unref(frame);
RenderLater(y);  // y 可能已经失效
```

`data[]` 只是指向底层图像缓冲区。执行 `av_frame_unref()` 或复用 `AVFrame` 后，如果没有其他引用持有缓冲区，这些指针就可能失效。

### 10.2 本项目如何解决

本项目对解码结果执行：

```cpp
AVFrame *cloned_frame = av_frame_clone(decoded_frame);
```

`av_frame_clone()` 会：

1. 创建一个新的 `AVFrame` 描述对象；
2. 增加底层图像缓冲区的引用计数；
3. 让克隆帧拥有独立的生命周期。

它通常**不是把整幅 YUV 图像重新复制一遍**，因此比手动深拷贝更适合实时视频。

项目再使用带自定义删除器的 `std::shared_ptr<void>` 持有这个 `AVFrame`：

```cpp
std::shared_ptr<void>(
    frame,
    [](void *pointer) {
        AVFrame *owned = static_cast<AVFrame *>(pointer);
        av_frame_free(&owned);
    });
```

这样网络、解码和渲染位于不同线程时，只要渲染队列中的帧仍然存在，底层 YUV 数据就不会被提前释放。

### 面试回答

> 解码器内部的 AVFrame 会被循环复用，所以我不能只把 `data[]` 裸指针交给渲染线程。我通过 `av_frame_clone` 增加底层 buffer 的引用计数，再用智能指针管理克隆帧。这样避免了整帧 memcpy，同时保证渲染期间数据有效。

---

## 11. AVPacket 的内存所有权

本项目使用一个栈上的 `AVPacket` 描述网络缓冲区：

```cpp
AVPacket packet{};
packet.data = const_cast<uint8_t *>(data);
packet.size = static_cast<int>(size);
avcodec_send_packet(codec_context, &packet);
```

这里的 packet 只是临时描述这段外部内存，并没有取得网络缓冲区的所有权。

关键约束是：

- `data` 在 `avcodec_send_packet()` 返回之前必须有效；
- 调用返回后，FFmpeg 会按其内部机制保留解码所需数据；
- 不能对这类外部内存直接调用会转移所有权的释放方式；
- 如果需要长期保存 packet，应使用 FFmpeg 的引用计数 API 正确创建或复制。

由于本项目在同一个解码线程内同步调用 `send_packet()`，网络帧在调用期间仍然有效，因此这种方式避免了一次额外的大块拼接和复制。

---

## 12. YUV420P、YUVJ420P 和 NV12

### 12.1 YUV420P

YUV420P 又称 I420，三个平面分开存放：

```text
[Y 平面][U 平面][V 平面]
```

对于 `640 x 480`：

```text
Y：640 x 480 = 307200 字节
U：320 x 240 =  76800 字节
V：320 x 240 =  76800 字节
总计：460800 字节，约为宽 x 高 x 1.5
```

### 12.2 NV12

NV12 的 Y 平面独立，UV 交错：

```text
[Y 平面][UVUVUV...]
```

数据量同样约为：

```text
width x height x 1.5
```

### 12.3 YUVJ420P

`YUVJ420P` 是 FFmpeg 历史上用于表示 full-range YUV420P 的像素格式。新版本更倾向于通过色彩范围字段描述 limited range 或 full range。

本项目把 `AV_PIX_FMT_YUV420P` 和 `AV_PIX_FMT_YUVJ420P` 都映射到内部的 YUV420P 渲染路径。

### 12.4 为什么不一定得到编码器输入的同名格式

编码器输入 I420，不意味着客户端只能输出 I420。解码器最终输出什么格式，取决于：

- 编码格式；
- 解码器实现；
- 软件或硬件解码后端；
- 像素格式协商。

因此客户端应该根据 `AVFrame::format` 判断真实输出格式，而不是写死。

---

## 13. linesize 为什么不一定等于 width

`linesize[i]` 是某个平面从当前行首走到下一行首需要跨过的字节数，也称为 stride。

为了 SIMD、缓存对齐或硬件要求，FFmpeg 可能在每行末尾添加填充：

```text
有效像素：640 字节
实际步长：672 字节

[640 字节有效数据][32 字节 padding]
```

因此逐行读取 Y 平面时，应写成：

```cpp
for (int row = 0; row < height; ++row) {
    const uint8_t *line = frame->data[0] + row * frame->linesize[0];
    ProcessOneLine(line, width);
}
```

不能简单假设：

```cpp
下一行 = 当前行 + width
```

OpenGL 上传纹理时也要正确处理 stride，否则可能出现画面倾斜、错位或颜色异常。

---

## 14. PTS、DTS 与 B 帧

### DTS

DTS 是解码时间戳，表示压缩帧应当何时送入解码器。

### PTS

PTS 是显示时间戳，表示解码后的帧应当何时展示。

### 为什么可能不相等

如果有 B 帧，编码顺序和显示顺序可能不同：

```text
显示顺序：I0  B1  B2  P3
解码顺序：I0  P3  B1  B2
```

因此 DTS 和 PTS 可能不相等。

本项目面向低延迟实时视频，编码侧通常关闭 B 帧和 lookahead。这样可以减少重排序等待，使编码、传输、解码和显示顺序更接近。

需要区分两类时间：

- 媒体时间戳：用于决定帧的播放顺序和时间；
- 性能时间戳：采集、编码、发送、接收、解码、渲染阶段记录的单调时钟，用于测量延迟。

两者用途不同，不应混为一谈。

---

## 15. 为什么本项目不使用 libavformat

如果输入是 MP4 文件，典型流程是：

```text
avformat_open_input()
        |
avformat_find_stream_info()
        |
找到视频流和对应解码器
        |
av_read_frame()
        |
得到 AVPacket
        |
avcodec_send_packet()
```

而本项目的流程是：

```text
TCP/UDP/RTP 收包
        |
项目自己的协议解析和帧重组
        |
得到完整 H.264 Annex-B 数据
        |
直接构造 AVPacket
        |
avcodec_send_packet()
```

所以项目不需要 FFmpeg 再解析 MP4、FLV 或 TS 容器。

### 面试追问：RTP 为什么也不用 libavformat

因为本项目自己实现了 RTP/H.264 接收逻辑：

- 解析 RTP Header；
- 根据 sequence number 检测丢包和乱序；
- 根据 timestamp 聚合同一视频帧；
- 处理单 NALU 和 FU-A；
- 恢复 Annex-B 起始码；
- 最后再交给 FFmpeg 解码。

这使网络策略和统计逻辑完全可控，但开发成本比直接使用成熟协议栈更高。

---

## 16. 解码失败时如何排查

建议按以下顺序定位：

### 16.1 检查输入是不是合法 H.264

- 是否存在 Annex-B 起始码；
- NALU 长度是否正确；
- RTP FU-A 重组时是否恢复了原始 NALU Header；
- UDP 分片是否缺失或偏移错误；
- 是否把协议头或 RTP Header 误送给了解码器。

### 16.2 检查 SPS、PPS 和 IDR

- 客户端是否从普通 P 帧中途加入；
- 服务端关键帧前是否重复参数集；
- 丢包是否破坏了 SPS、PPS 或 IDR；
- 解码报错后是否一直把不可恢复的依赖帧送入解码器。

### 16.3 检查 API 调用循环

- `send_packet()` 后是否循环 `receive_frame()`；
- 是否把 `EAGAIN` 错误地当成解码失败；
- `AVFrame` 使用后是否正确 `unref`；
- 重连后是否清理旧解码状态。

### 16.4 检查图像格式和步长

如果解码成功但画面异常：

- 检查 `AVFrame::format`；
- 检查 `data[]` 平面顺序；
- 检查 `linesize[]`；
- 检查 OpenGL 的纹理尺寸和 shader 采样；
- 检查 YUV 色彩范围和色彩矩阵。

---

## 17. 实时视频如何降低 FFmpeg 相关延迟

低延迟不是只设置一个 FFmpeg 参数，而是整条链路共同控制。

### 编码端

- 关闭 B 帧；
- 关闭 lookahead；
- 使用 `zerolatency` tune；
- 选择合适的 preset；
- 控制 GOP，避免关键帧间隔过长；
- 避免编码帧在队列中长期积压。

### 网络端

- 采用适合业务的 TCP、UDP 或 RTP；
- UDP/RTP 正确处理分片、丢包、乱序和抖动；
- Jitter Buffer 不宜盲目设置过大；
- 网络损坏后尽快等到或请求关键帧。

### 解码端

- 正确使用 send/receive API，及时取出全部输出；
- 不做没有必要的整帧内存复制；
- 使用引用计数管理跨线程帧生命周期；
- 队列有界，积压时优先保留较新的可解码帧；
- 重连或码流切换时清理旧参考状态；
- 根据 CPU 核数和低延迟目标评估解码线程数。

### 渲染端

- 直接把 YUV 平面上传为纹理；
- 在 fragment shader 中完成 YUV 到 RGB；
- 避免先在 CPU 中转换出一份完整 RGB 图像；
- 实时场景优先显示最新帧，避免播放历史积压画面。

---

## 18. 软件解码与硬件解码

### 软件解码

由 CPU 执行 H.264 解码。

优点：

- 跨平台行为相对统一；
- 部署和调试简单；
- 输出格式容易处理；
- 小分辨率或低帧率下通常足够。

缺点：

- 高分辨率、高帧率或多路视频时 CPU 压力较大；
- 功耗通常高于专用硬件。

### 硬件解码

通过 GPU 或专用视频单元解码，例如：

- NVIDIA NVDEC；
- Intel VAAPI/QSV；
- Linux V4L2 M2M；
- Windows D3D11VA；
- Apple VideoToolbox。

优点：

- 降低 CPU 占用；
- 更适合 1080p/4K、多路视频和低功耗设备。

难点：

- 平台差异大；
- 硬件帧可能位于 GPU 内存；
- 需要处理硬件像素格式、映射和上下文；
- 如果先下载回 CPU 再上传 OpenGL，可能抵消一部分收益。

### 本项目如何回答

> 当前项目优先实现 FFmpeg 软件解码，因为 640x480 单路视频下性能足够，而且便于先验证网络到渲染的完整链路。后续如果扩展到机器人端多路 1080p 视频，我会增加硬件解码后端，并尽量让解码输出直接进入 GPU 渲染路径，减少 CPU 和 GPU 之间的数据搬运。

---

## 19. 本项目解码模块的完整业务流程

1. 客户端网络线程接收 TCP、UDP 或 RTP 数据。
2. 网络模块完成协议解析、分片重组和帧边界恢复。
3. 网络模块向解码队列提交一份完整 H.264 Annex-B 帧。
4. 解码线程取出压缩帧，构造临时 `AVPacket`。
5. 调用 `avcodec_send_packet()` 把 H.264 数据送入解码器。
6. 循环调用 `avcodec_receive_frame()` 取出所有可用图像。
7. 检查输出宽高和像素格式。
8. 对最新的 `AVFrame` 建立引用计数所有权。
9. 把宽高、格式、平面指针和 stride 封装成项目自己的解码帧。
10. 将解码帧放入渲染队列。
11. OpenGL 根据 YUV420P 或 NV12 上传纹理并在 GPU 中转换为 RGB。
12. 渲染线程交换前后缓冲区，把完整画面显示到窗口。

---

## 20. 一分钟面试口述版本

> 客户端网络模块会先把 TCP、UDP 或 RTP 收到的数据恢复成完整的 H.264 Annex-B 帧，然后交给独立的解码线程。我在解码层使用 FFmpeg 的 `libavcodec`：初始化时查找 H.264 解码器，创建并打开解码上下文；运行时把压缩码流包装成 `AVPacket`，通过 `avcodec_send_packet` 输入，再循环调用 `avcodec_receive_frame` 获取解码后的 YUV 图像。因为一次输入不一定恰好输出一帧，所以需要正确处理多次 receive、EAGAIN 和 EOF。解码器内部的 AVFrame 会被复用，为了把图像安全交给渲染线程，我通过 `av_frame_clone` 持有底层 buffer 的引用计数，再使用智能指针管理生命周期，这样既避免整帧深拷贝，也不会出现悬空指针。最后根据实际像素格式，把 YUV420P 或 NV12 的平面指针和 stride 交给 OpenGL，在 GPU shader 中完成 YUV 到 RGB 转换和显示。

---

## 21. 高频面试问题与参考答案

### 1. FFmpeg 是什么？

> FFmpeg 是一套开源音视频处理工具和库集合。命令行工具可用于转码、裁剪和推流，开发库则提供解封装、编解码、滤镜、缩放和重采样等能力。我项目里直接使用 `libavcodec` 解码网络传来的 H.264 裸码流。

### 2. AVPacket 和 AVFrame 有什么区别？

> AVPacket 保存压缩数据，例如 H.264 NALU；AVFrame 保存解码后的原始图像，例如 YUV420P。解码过程就是把 packet 输入解码器，再从解码器取出 frame。

### 3. 一个 AVPacket 一定对应一个 AVFrame 吗？

> 不一定。由于编码组织、缓存和帧重排，一个 packet 可能输出零帧、一帧或多帧，所以 send 后必须循环 receive，直到返回 EAGAIN。

### 4. EAGAIN 是错误吗？

> 通常不是。在 receive 阶段，它表示当前输出已经取完，需要输入更多 packet；在 send 阶段，它表示还有输出没有取完，应先 receive。

### 5. 为什么使用 av_frame_clone？

> 解码器内部 AVFrame 会被复用，直接保存其 data 裸指针可能在下一次解码后失效。`av_frame_clone` 会创建新的帧描述并增加底层 buffer 的引用计数，能保证跨线程渲染期间数据有效，又避免整帧深拷贝。

### 6. av_frame_clone 是深拷贝吗？

> 通常不是。它主要复制帧描述并引用同一组引用计数缓冲区。若确实需要独立像素内存，应另行分配缓冲区并复制图像数据。

### 7. 为什么 linesize 可能大于 width？

> FFmpeg 为满足 SIMD、缓存或硬件对齐要求，可能在每行末尾添加 padding。因此访问下一行必须使用 `data + row * linesize`，不能始终使用 width 计算。

### 8. 项目为什么没有使用 libavformat？

> 因为网络模块已经把自定义 TCP/UDP 协议或 RTP 数据恢复成 H.264 Annex-B 裸码流，没有 MP4、FLV 等容器需要解封装，所以可以直接使用 `libavcodec`。

### 9. 为什么会出现 non-existing PPS referenced？

> 常见原因是客户端从普通 P 帧中途加入，还没有收到 SPS/PPS；也可能是参数集或关键帧在网络中损坏。解决思路是关键帧前重复 SPS/PPS、等待或请求新 IDR，并在重连或码流切换时刷新解码器状态。

### 10. 解码成功为什么可能没有输出画面？

> `avcodec_send_packet` 成功只说明输入被接受，不代表立刻有 AVFrame 输出。解码器可能仍在等待后续数据或参考帧，需要继续输入并循环 receive。

### 11. 为什么不用 CPU 把 YUV 转成 RGB？

> CPU 转换会产生额外计算和一份 RGB 内存。本项目直接上传 YUV 平面，在 OpenGL fragment shader 中并行完成 YUV 到 RGB，减少 CPU 工作和不必要的数据复制。

### 12. B 帧为什么增加延迟？

> B 帧可能依赖前后参考帧，解码器需要等待未来参考帧并进行显示重排，因此增加缓存和等待。实时低延迟项目通常关闭 B 帧和 lookahead。

### 13. 什么时候调用 avcodec_flush_buffers？

> 发生 seek、重连、码流切换、时间线跳变，或需要丢弃旧参考状态时可以调用。但刷新后通常要从新的 SPS/PPS 和关键帧重新建立解码状态。

### 14. FFmpeg 是否线程安全？

> 不能简单回答“全部线程安全”。不同解码上下文可以放在不同线程中独立使用，但同一个 `AVCodecContext` 不应被多个业务线程无同步地同时调用。本项目由单独解码线程串行操作一个上下文。

### 15. 如何避免 FFmpeg 资源泄漏？

> 明确每类对象的所有权，`AVFrame` 用 `av_frame_free`，`AVCodecContext` 用 `avcodec_free_context`，使用后及时 `av_frame_unref` 或 `av_packet_unref`。跨线程时用 RAII 或自定义智能指针删除器管理 FFmpeg 对象。

### 16. 如果要支持 MP4 文件播放，需要增加什么？

> 需要使用 `libavformat` 打开输入、读取流信息、选择视频流，并通过 `av_read_frame` 获取 packet；再根据 time base、PTS 和播放时钟进行同步，最后送给 `libavcodec` 解码。

### 17. 如果要支持音频，还需要考虑什么？

> 需要选择音频流并解码，通过 `libswresample` 统一采样率、采样格式和声道布局，同时建立音视频同步时钟。常见策略是以音频时钟为主，视频根据 PTS 等待或丢帧。

### 18. 软件解码性能不够怎么办？

> 先用统计确认瓶颈是否真的在解码，再考虑解码线程配置、减少格式转换和复制；高分辨率或多路场景可以接入 NVDEC、VAAPI、QSV 等硬件解码，并设计 GPU 内存到 OpenGL 的高效互操作路径。

---

## 22. 常见错误说法

### 错误 1：FFmpeg 就是一个播放器

正确说法：

> FFmpeg 是工具和开发库集合，本身可作为音视频基础设施使用。

### 错误 2：MP4 是一种视频编码

正确说法：

> MP4 是封装格式，H.264/H.265 才是常见视频编码格式。

### 错误 3：send_packet 成功就一定解出一帧

正确说法：

> send 和 receive 是解耦的，一次输入可能产生零帧、一帧或多帧。

### 错误 4：EAGAIN 表示解码器坏了

正确说法：

> EAGAIN 是流式 API 的正常状态，用于提示输入与输出之间的切换。

### 错误 5：data[0] 后面一定是连续的整幅图像

正确说法：

> 平面格式有多个 data 指针，每行还要按照 linesize 访问。

### 错误 6：av_frame_clone 会完整复制整幅图像

正确说法：

> 它通常增加引用计数并共享底层 buffer，不等于像素深拷贝。

### 错误 7：低延迟只要给 FFmpeg 设置一个参数

正确说法：

> 低延迟是采集、编码、队列、网络、解码和渲染共同决定的。

---

## 23. 面试前速记表

```text
FFmpeg              音视频工具和开发库集合
libavcodec           编码、解码
libavformat          封装、解封装
libavutil            公共工具、错误、像素格式
libswscale           缩放、像素格式转换

AVCodec              解码器描述
AVCodecContext       一路流的解码器实例与状态
AVPacket             压缩数据
AVFrame              解码后的原始数据

send_packet          输入压缩数据
receive_frame        获取解码结果
EAGAIN               当前阶段需要切换输入/输出
EOF                  解码器已经排空

YUV420P              Y、U、V 三平面
NV12                 Y 平面 + UV 交错平面
linesize             相邻两行首地址之间的字节跨度

SPS/PPS              解码参数
IDR                  可重新开始解码的关键图像
PTS                  显示时间戳
DTS                  解码时间戳

项目输入              完整 H.264 Annex-B 帧
项目输出              YUV420P 或 NV12
生命周期策略          av_frame_clone + shared_ptr
渲染策略              OpenGL shader 完成 YUV -> RGB
```

---

## 24. 推荐复习顺序

1. 先讲清编码、封装和裸码流的区别。
2. 记住 `AVCodec`、`AVCodecContext`、`AVPacket`、`AVFrame`。
3. 手写 send/receive 解码循环。
4. 彻底理解 `EAGAIN`。
5. 理解 YUV420P、NV12、data 和 linesize。
6. 理解 AVFrame 引用计数和跨线程生命周期。
7. 结合本项目讲清网络帧如何进入 FFmpeg。
8. 准备 SPS/PPS 缺失、丢包、重连和低延迟追问。
9. 最后练习一分钟口述版本，不要只背 API 名称。

只要能围绕下面这条主线展开，FFmpeg 基础面试就不会乱：

```text
压缩数据从哪里来
    -> 如何构造成 AVPacket
    -> 如何送入解码器
    -> 如何取出 AVFrame
    -> 如何管理图像生命周期
    -> 如何根据格式和 stride 交给渲染器
```
