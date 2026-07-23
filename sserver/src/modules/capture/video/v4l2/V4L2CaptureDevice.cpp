#include "modules/capture/video/v4l2/V4L2CaptureDevice.h"

#include <arpa/inet.h>
#include <asm/types.h>
#include <errno.h>
#include <fcntl.h>
#include <malloc.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

#include "common/log/Logger.h"
#include "common/time/MonotonicClock.h"

namespace sserver {
namespace modules {
namespace capture {
namespace v4l2 {

namespace {

// V4L2 buffer timestamp 使用 timeval，项目内部统一使用纳秒时间戳。
std::uint64_t TimevalToNs(const timeval &timestamp) {
    return static_cast<std::uint64_t>(timestamp.tv_sec) * 1000000000ULL +
           static_cast<std::uint64_t>(timestamp.tv_usec) * 1000ULL;
}

}  // namespace

V4L2CaptureDevice::V4L2CaptureDevice(
        const config::CaptureConfig &capture_config,
        const config::CodecConfig &codec_config)
        : capture_config_(capture_config),
          codec_config_(codec_config),
          device_fd_(-1),
          opened_(false),
          streaming_(false),
          logged_timestamp_source_(false),
          sequence_(0) {
}

V4L2CaptureDevice::~V4L2CaptureDevice() {
    Stop();
    Close();
}

bool V4L2CaptureDevice::Open() {
    // 先确认配置的路径存在，并且确实是字符设备。
    // Linux 摄像头通常暴露为 /dev/video0 这类字符设备。
    struct stat device_stat{};

    // 检查路径是否存在
    if (stat(capture_config_.device.c_str(), &device_stat) < 0) {
        common::log::Logger::Error("cannot identify video device: " + capture_config_.device);
        return false;
    }

    // 检查是否是字符设备
    if (!S_ISCHR(device_stat.st_mode)) {
        common::log::Logger::Error("configured capture device is not a character device: " + capture_config_.device);
        return false;
    }

    // 打开摄像头设备。后续所有 VIDIOC_* ioctl 都依赖这个 fd。
    device_fd_ = open(capture_config_.device.c_str(), O_RDWR, 0);
    if (device_fd_ < 0) {
        common::log::Logger::Error("failed to open capture device: " + capture_config_.device);
        return false;
    }

    opened_ = true;
    return true;
}

// 启动 V4L2 采集设备：依次完成能力查询、格式配置、帧率设置、内存映射、缓冲区入队
bool V4L2CaptureDevice::Start() {
    // 启动 V4L2 采集设备的完整准备流程：
    //   查询能力 -> 设置像素格式 -> 设置帧率 -> 申请/映射 buffer
    //   -> buffer 入队 -> 初始化编码器 -> STREAMON。
    if (!opened_) {
        return false;
    }

    if (!QueryCapabilities() || !ConfigureFormat() || !ConfigureFrameRate() || !InitializeMemoryMapping() ||
        !QueueCaptureBuffers()) {
        ReleaseMappedBuffers();
        return false;
    }

    // 摄像头输出原始 YUYV422，网络传输需要 H.264，所以这里创建视频编码器。
    encoder_ = std::make_unique<modules::encoding::VideoEncoder>();

    std::string error_message;
    if (!encoder_->Initialize(
                capture_config_.width,
                capture_config_.height,
                capture_config_.fps,
                codec_config_,
                &error_message)) {
        if (!error_message.empty()) {
            common::log::Logger::Error("failed to initialize video encoder: " + error_message);
        }
        ReleaseMappedBuffers();
        encoder_.reset();
        return false;
    }

    if (!StartStreaming()) {
        encoder_->Shutdown();
        encoder_.reset();
        ReleaseMappedBuffers();
        return false;
    }

    streaming_ = true;
    return true;
}

common::model::EncodedFramePtr V4L2CaptureDevice::CaptureFrame() {
    // 如果当前设备还没有进入 streaming 状态，说明摄像头采集流未启动
    if (!streaming_) {
        return common::model::EncodedFramePtr();
    }

    // 单线程路径：DQBUF 取一帧 -> x264 编码 -> QBUF 归还 buffer -> 返回 EncodedFrame。

    // v4l2_buffer 用于描述一个 V4L2 采集缓冲区。
    //
    // 在 mmap 采集模式下，摄像头采集到的数据会先写入驱动 buffer。
    // 用户态通过 VIDIOC_DQBUF 从驱动队列中取出一个已经填充好的 buffer。
    struct v4l2_buffer buffer{};
    buffer.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    buffer.memory = V4L2_MEMORY_MMAP;

    // VIDIOC_DQBUF：从驱动已填好的队列中取出一个 buffer。
    // 调用成功后：
    // 1. buffer.index 表示是哪一个 mmap buffer；
    // 2. buffer.bytesused 表示本次采集帧实际使用了多少字节；
    // 3. buffer.timestamp / buffer.flags 可能携带驱动或硬件提供的采集时间戳
    if (IoctlWithRetry(VIDIOC_DQBUF, &buffer) < 0) {
        // 这不是致命错误，只表示当前还没有新帧可取
        if (errno == EAGAIN) {
            return common::model::EncodedFramePtr();
        }
        common::log::Logger::Warn("VIDIOC_DQBUF failed");
        return common::model::EncodedFramePtr();
    }

    // 采集时间戳优先使用 V4L2 提供的 monotonic buffer timestamp。
    // 如果驱动不提供，就退化为当前 dequeue 时刻。
    std::uint64_t capture_timestamp_ns = common::time::MonotonicNowNs();

    // 判断 V4L2 buffer 是否携带单调时钟时间戳
    const bool has_monotonic_timestamp =
            (buffer.flags & V4L2_BUF_FLAG_TIMESTAMP_MASK) == V4L2_BUF_FLAG_TIMESTAMP_MONOTONIC;

    // 如果驱动提供了单调时间戳，则优先使用驱动/硬件记录的采集时间
    if (has_monotonic_timestamp) {
        capture_timestamp_ns = TimevalToNs(buffer.timestamp);
    }

    // 只在第一次采集时打印一次时间戳来源日志，避免每帧都打印造成日志刷屏
    if (!logged_timestamp_source_) {
        if (has_monotonic_timestamp) {
            common::log::Logger::Info("v4l2 capture is using hardware monotonic buffer timestamps");
        } else {
            common::log::Logger::Warn("v4l2 capture did not expose monotonic buffer timestamps, falling back to dequeue timestamps");
        }
        logged_timestamp_source_ = true;
    }

    // encoded_payload 用于保存编码后的 H.264 数据。
    std::vector<std::uint8_t> encoded_payload;
    bool is_keyframe = false;

    // buffer.index 指向 mmap 后的用户态地址。
    const void *start = buffers_[buffer.index].start;

    // 本次有效数据长度优先使用 buffer.bytesused
    const std::size_t length = buffer.bytesused > 0 ? buffer.bytesused : buffers_[buffer.index].length;

    const std::uint64_t encode_start_timestamp_ns = common::time::MonotonicNowNs();
    std::string error_message;
    // 编码器期望输入 YUYV422，内部会先转 I420，再调用 x264。
    if (encoder_ == nullptr ||
        !encoder_->EncodeYuyv422Frame(
                static_cast<const std::uint8_t *>(start),
                length,
                &encoded_payload,
                &is_keyframe,
                &error_message)) {
        if (!error_message.empty()) {
            common::log::Logger::Warn("video encoder dropped frame: " + error_message);
        }
        // 编码失败也必须把 buffer 还给驱动，否则摄像头可用 buffer 会越来越少。
        IoctlWithRetry(VIDIOC_QBUF, &buffer);
        return common::model::EncodedFramePtr();
    }
    const std::uint64_t encode_end_timestamp_ns = common::time::MonotonicNowNs();

    // VIDIOC_QBUF：把 buffer 归还给驱动，让摄像头继续写后续帧。
    if (IoctlWithRetry(VIDIOC_QBUF, &buffer) < 0) {
        common::log::Logger::Warn("VIDIOC_QBUF failed");
        return common::model::EncodedFramePtr();
    }

    // 封装成项目统一的 EncodedFrame，后续 TransportModule 只处理这个结构。
    auto frame = std::make_shared<common::model::EncodedFrame>();
    frame->sequence = sequence_++;
    frame->type = common::model::StreamPayloadType::kVideo;
    frame->capture_timestamp_ns = capture_timestamp_ns;
    frame->encode_start_timestamp_ns = encode_start_timestamp_ns;
    frame->encode_end_timestamp_ns = encode_end_timestamp_ns;
    frame->is_keyframe = is_keyframe;
    frame->payload.swap(encoded_payload);
    return frame;
}

bool V4L2CaptureDevice::SupportsRawCapture() const {
    // V4L2 支持 CaptureModule 的双线程 raw capture 路径。
    return true;
}

RawCaptureFramePtr V4L2CaptureDevice::CaptureRawFrame() {
    // 如果当前设备还没有进入 streaming 状态，说明摄像头采集流未启动
    if (!streaming_) {
        return nullptr;
    }

    // 双线程路径的采集阶段：只做 DQBUF、复制原始数据、QBUF，不做编码。

    // v4l2_buffer 用于描述一个 V4L2 采集缓冲区。
    //
    // 在 mmap 采集模式下，摄像头采集到的数据会先写入驱动 buffer。
    // 用户态通过 VIDIOC_DQBUF 从驱动队列中取出一个已经填充好的 buffer。
    struct v4l2_buffer buffer{};
    buffer.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    buffer.memory = V4L2_MEMORY_MMAP;

    // 从驱动队列中取出一个已经采集完成的 buffer。
    // 调用成功后：
    // 1. buffer.index 表示是哪一个 mmap buffer；
    // 2. buffer.bytesused 表示本次采集帧实际使用了多少字节；
    // 3. buffer.timestamp / buffer.flags 可能携带驱动或硬件提供的采集时间戳
    if (IoctlWithRetry(VIDIOC_DQBUF, &buffer) < 0) {
        // 这不是致命错误，只表示当前还没有新帧可取
        if (errno == EAGAIN) {
            return nullptr;
        }
        common::log::Logger::Warn("VIDIOC_DQBUF failed");
        return nullptr;
    }

    // 创建一个原始采集帧对象
    auto raw = std::make_shared<RawCaptureFrame>();
    raw->capture_timestamp_ns = common::time::MonotonicNowNs();

    // 判断 V4L2 buffer 是否携带单调时钟时间戳
    const bool has_monotonic_timestamp =
            (buffer.flags & V4L2_BUF_FLAG_TIMESTAMP_MASK) == V4L2_BUF_FLAG_TIMESTAMP_MONOTONIC;

    // 如果驱动提供了单调时间戳，则优先使用驱动/硬件记录的采集时间
    if (has_monotonic_timestamp) {
        raw->capture_timestamp_ns = TimevalToNs(buffer.timestamp);
    }

    // 只在第一次采集时打印一次时间戳来源日志，避免每帧都打印造成日志刷屏
    if (!logged_timestamp_source_) {
        if (has_monotonic_timestamp) {
            common::log::Logger::Info("v4l2 capture is using hardware monotonic buffer timestamps");
        } else {
            common::log::Logger::Warn("v4l2 capture did not expose monotonic buffer timestamps, falling back to dequeue timestamps");
        }
        logged_timestamp_source_ = true;
    }

    // 复制数据后立刻 QBUF，把内核 buffer 还给驱动。
    // 这样摄像头采集不必等待编码线程。

    // 根据 buffer.index 找到本次出队的 mmap buffer 起始地址。
    const void *start = buffers_[buffer.index].start;

    // 本次有效数据长度优先使用 buffer.bytesused， 表示驱动本次实际写入的字节数。
    const std::size_t length = buffer.bytesused > 0 ? buffer.bytesused : buffers_[buffer.index].length;

    // 将 mmap buffer 中的原始图像数据复制到 raw->data。
    //
    // 注意这里选择了“复制数据”而不是直接引用 mmap buffer。
    // 这样做的目的：
    // 1. 采集线程可以尽快把 buffer 还给驱动；
    // 2. 编码线程可以慢慢处理 raw->data；
    // 3. 避免编码线程占用 V4L2 内核 buffer 导致采集阻塞或丢帧。
    raw->data.assign(
            static_cast<const std::uint8_t *>(start),
            static_cast<const std::uint8_t *>(start) + length);
    raw->bytes_used = length;

    // 调用成功后，该 buffer 的所有权重新交给驱动，后续摄像头可以继续向这个 buffer 写入新的采集帧。
    IoctlWithRetry(VIDIOC_QBUF, &buffer);

    // 返回复制出来的原始帧。后续编码线程可以对该 raw frame 执行 YUYV422 -> I420 -> H.264 编码。
    return raw;
}

common::model::EncodedFramePtr V4L2CaptureDevice::EncodeRawFrame(RawCaptureFramePtr raw) {
    // 双线程路径的编码阶段：输入是 CaptureRawFrame() 复制出来的原始 YUYV 数据。
    if (!raw || encoder_ == nullptr) {
        return nullptr;
    }

    // 用于保存编码后的 H.264 payload
    std::vector<std::uint8_t> encoded_payload;

    // 标记当前编码输出是否为关键帧。
    //
    // 对 H.264 来说，关键帧通常对应 IDR 帧。
    // 关键帧对播放器首帧解码、丢包恢复、GOP 切换等都很重要。
    bool is_keyframe = false;

    const std::uint64_t encode_start_timestamp_ns = common::time::MonotonicNowNs();
    std::string error_message;
    // YUYV422 -> I420 -> x264 -> H.264 payload。
    if (!encoder_->EncodeYuyv422Frame(
                raw->data.data(),
                raw->bytes_used,
                &encoded_payload,
                &is_keyframe,
                &error_message)) {
        if (!error_message.empty()) {
            common::log::Logger::Warn("video encoder dropped frame: " + error_message);
        }
        return nullptr;
    }
    const std::uint64_t encode_end_timestamp_ns = common::time::MonotonicNowNs();

    // 保留原始采集时间戳，同时记录编码开始/结束时间戳。
    auto frame = std::make_shared<common::model::EncodedFrame>();
    frame->sequence = sequence_++;
    frame->type = common::model::StreamPayloadType::kVideo;
    frame->capture_timestamp_ns = raw->capture_timestamp_ns;
    frame->encode_start_timestamp_ns = encode_start_timestamp_ns;
    frame->encode_end_timestamp_ns = encode_end_timestamp_ns;
    frame->is_keyframe = is_keyframe;

    // 将编码后的 H.264 payload 移交给 EncodedFrame。
    //
    // 使用 swap 可以避免一次额外的数据拷贝。
    // swap 后 encoded_payload 变为空，frame->payload 持有编码数据。
    frame->payload.swap(encoded_payload);
    return frame;
}

void V4L2CaptureDevice::Stop() {
    if (!opened_) {
        return;
    }

    // 先停 V4L2 streaming，再关闭编码器和 mmap buffer。
    if (streaming_) {
        StopStreaming();
        streaming_ = false;
    }

    // 关闭编码器
    if (encoder_ != nullptr) {
        encoder_->Shutdown();
        encoder_.reset();
    }

    // 释放 mmap buffer
    ReleaseMappedBuffers();
}

void V4L2CaptureDevice::Close() {
    if (device_fd_ >= 0) {
        close(device_fd_);
        device_fd_ = -1;
    }
    opened_ = false;
}

std::string V4L2CaptureDevice::Describe() const {
    return "v4l2-capture-device(" + capture_config_.device + ")";
}

int V4L2CaptureDevice::IoctlWithRetry(unsigned long request, void *arg) {
    // 对 ioctl 进行一层封装，自动处理 EINTR 错误。
    //
    // ioctl 是 Linux 下控制设备的重要系统调用。
    // V4L2 中大量操作都依赖 ioctl，例如：
    //      VIDIOC_QUERYCAP    查询设备能力
    //      VIDIOC_S_FMT       设置采集格式
    //      VIDIOC_REQBUFS     申请 buffer
    //      VIDIOC_QBUF        buffer 入队
    //      VIDIOC_DQBUF       buffer 出队
    //      VIDIOC_STREAMON    启动采集
    //      VIDIOC_STREAMOFF   停止采集
    //
    // 参数说明：
    //      request：具体 ioctl 命令，例如 VIDIOC_DQBUF；
    //      arg    ：传给 ioctl 的参数结构体指针，例如 v4l2_buffer*。
    //
    // 返回值：
    //      >= 0：调用成功；
    //      < 0 ：调用失败，失败原因保存在 errno 中。
    int result = -1;
    do {
        result = ioctl(device_fd_, request, arg);
    } while (result < 0 && errno == EINTR);
    return result;
}

bool V4L2CaptureDevice::QueryCapabilities() {
    // 查询设备能力，确认它既支持视频采集，也支持 streaming I/O。
    struct v4l2_capability capability{};
    if (IoctlWithRetry(VIDIOC_QUERYCAP, &capability) < 0) {
        common::log::Logger::Error("VIDIOC_QUERYCAP failed");
        return false;
    }

    // 判断设备是否支持视频采集能力
    if ((capability.capabilities & V4L2_CAP_VIDEO_CAPTURE) == 0) {
        common::log::Logger::Error("device does not support video capture");
        return false;
    }

    // 判断设备是否支持 streaming I/O
    if ((capability.capabilities & V4L2_CAP_STREAMING) == 0) {
        common::log::Logger::Error("device does not support streaming I/O");
        return false;
    }

    return true;
}

bool V4L2CaptureDevice::ConfigureFormat() {
    // 设置摄像头输出格式。
    // 当前项目固定要求 V4L2 输出 YUYV422，然后编码器再转 I420 给 x264。
    struct v4l2_format format{};

    // 指定当前配置的是视频采集流
    format.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    format.fmt.pix.width = capture_config_.width;
    format.fmt.pix.height = capture_config_.height;

    // 设置摄像头输出像素格式为 YUYV422。
    format.fmt.pix.pixelformat = V4L2_PIX_FMT_YUYV;

    // 设置视频场类型。
    // V4L2_FIELD_INTERLACED 表示交错场格式。
    format.fmt.pix.field = V4L2_FIELD_INTERLACED;

    // 将上述格式配置写入摄像头驱动
    if (IoctlWithRetry(VIDIOC_S_FMT, &format) < 0) {
        common::log::Logger::Error("VIDIOC_S_FMT failed");
        return false;
    }

    return true;
}

bool V4L2CaptureDevice::ConfigureFrameRate() {
    // 尝试设置采集帧率。
    // 有些摄像头/驱动不支持设置 timeperframe，因此失败时只告警并继续使用默认值。

    // 创建 V4L2 流参数结构体，并使用 {} 初始化为 0
    struct v4l2_streamparm stream_parameters{};

    // 指定当前操作的是“视频采集”类型
    stream_parameters.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;

    // 通过 VIDIOC_G_PARM 获取当前摄像头的流参数
    if (IoctlWithRetry(VIDIOC_G_PARM, &stream_parameters) < 0) {
        common::log::Logger::Warn("VIDIOC_G_PARM failed, skipping frame rate configuration");
        return true;
    }

    // 检查设备是否支持通过 timeperframe 设置帧间隔。
    //
    // V4L2_CAP_TIMEPERFRAME 表示驱动支持配置每帧时间，
    // 也就是可以通过设置 timeperframe 来间接设置 fps。
    //
    // capability 是一个位掩码，因此这里使用按位与 & 判断是否包含该能力。
    if ((stream_parameters.parm.capture.capability & V4L2_CAP_TIMEPERFRAME) == 0) {
        common::log::Logger::Warn("capture device does not expose V4L2_CAP_TIMEPERFRAME");
        return true;
    }

    // 设置每一帧的时间间隔。
    //
    // timeperframe 是一个分数：
    //      每帧时间 = numerator / denominator 秒
    stream_parameters.parm.capture.timeperframe.numerator = 1;
    stream_parameters.parm.capture.timeperframe.denominator = static_cast<unsigned int>(capture_config_.fps);

    // 通过 VIDIOC_S_PARM 将修改后的流参数写回摄像头驱动。
    if (IoctlWithRetry(VIDIOC_S_PARM, &stream_parameters) < 0) {
        common::log::Logger::Warn("VIDIOC_S_PARM failed, continuing with driver default fps");
        return true;
    }

    return true;
}

bool V4L2CaptureDevice::InitializeMemoryMapping() {
    // 请求 V4L2 分配若干内核 buffer，并通过 mmap 映射到用户态。
    // 摄像头写入内核 buffer 后，用户态可以直接读取映射内存。

    // 在 streaming I/O 模式下，摄像头采集到的数据不会直接返回给用户态，而是先写入驱动管理的内核缓冲区中。
    // 用户态随后通过 mmap 将这些内核缓冲区映射到自己的进程地址空间，从而避免频繁 read() 拷贝，提高采集效率。
    struct v4l2_requestbuffers request{};

    // 请求驱动分配的 buffer 数量
    request.count = static_cast<unsigned int>(capture_config_.device_buffer_count);
    // 指定 buffer 类型为视频采集
    request.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    // 指定使用 mmap 方式访问 buffer
    request.memory = V4L2_MEMORY_MMAP;

    // 通过 VIDIOC_REQBUFS 向驱动申请采集缓冲区
    if (IoctlWithRetry(VIDIOC_REQBUFS, &request) < 0) {
        common::log::Logger::Error("VIDIOC_REQBUFS failed");
        return false;
    }

    // 至少需要 2 个 buffer 才比较合理。
    // 多 buffer 的意义是：当用户态正在处理一个 buffer 时，驱动仍然可以向其他 buffer 写入新帧，这样可以降低丢帧概率。
    if (request.count < 2) {
        common::log::Logger::Error("insufficient capture buffers");
        return false;
    }

    // 根据驱动实际分配的 buffer 数量调整用户态 buffers_ 数组
    buffers_.clear();
    buffers_.resize(request.count);

    // 逐个查询并映射每一个 V4L2 buffer
    for (std::size_t index = 0; index < buffers_.size(); ++index) {

        // v4l2_buffer 用于描述单个 V4L2 buffer 的信息
        struct v4l2_buffer buffer{};
        buffer.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
        buffer.memory = V4L2_MEMORY_MMAP;

        // 指定当前要查询第几个 buffer
        buffer.index = static_cast<unsigned int>(index);

        // 查询第 index 个 buffer 的详细信息。
        //
        // VIDIOC_QUERYBUF 会由驱动填充 buffer.length 和 buffer.m.offset。
        //
        // buffer.length:
        //      当前 buffer 的内存长度。
        //
        // buffer.m.offset:
        //      当前 buffer 在设备内存中的偏移量。
        //      后续 mmap 需要使用这个 offset 来完成映射
        if (IoctlWithRetry(VIDIOC_QUERYBUF, &buffer) < 0) {
            common::log::Logger::Error("VIDIOC_QUERYBUF failed");
            return false;
        }

        // 保存当前 buffer 的长度，后续读取帧数据和 munmap 释放时都需要用到
        buffers_[index].length = buffer.length;

        // 将驱动分配的内核 buffer 映射到当前进程的用户态地址空间。
        //
        // mmap 参数含义：
        // 1. nullptr:
        //      让系统自动选择映射起始地址。
        //
        // 2. buffer.length:
        //      映射区域长度。
        //
        // 3. PROT_READ | PROT_WRITE:
        //      映射后的内存允许读写。
        //
        // 4. MAP_SHARED:
        //      用户态和内核态共享这块内存。
        //      摄像头写入 buffer 后，用户态可以直接读取。
        //
        // 5. device_fd_:
        //      摄像头设备文件描述符。
        //
        // 6. buffer.m.offset:
        //      当前 buffer 在设备中的偏移量，由 VIDIOC_QUERYBUF 获得。
        buffers_[index].start = mmap(nullptr, buffer.length, PROT_READ | PROT_WRITE, MAP_SHARED, device_fd_, buffer.m.offset);

        // mmap 失败时返回 MAP_FAILED。
        // 为避免后续误用非法地址，这里将 start 重新置为 nullptr。
        if (buffers_[index].start == MAP_FAILED) {
            buffers_[index].start = nullptr;
            common::log::Logger::Error("mmap failed for capture buffer");
            return false;
        }
    }

    return true;
}

bool V4L2CaptureDevice::QueueCaptureBuffers() {
    // 把所有 mmap buffer 放入驱动队列。STREAMON 后摄像头会往这些 buffer 写帧。

    // 注意：
    // mmap 只是完成“内核 buffer 到用户态地址空间”的映射，但 buffer 还没有交给驱动使用。
    // 必须通过 VIDIOC_QBUF 将 buffer 入队后，驱动才能在 STREAMON 之后把摄像头采集到的帧写入这些 buffer。
    for (std::size_t index = 0; index < buffers_.size(); ++index) {
        struct v4l2_buffer buffer{};
        buffer.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
        buffer.memory = V4L2_MEMORY_MMAP;
        buffer.index = static_cast<unsigned int>(index);

        // 将当前 buffer 放入驱动采集队列
        if (IoctlWithRetry(VIDIOC_QBUF, &buffer) < 0) {
            common::log::Logger::Error("VIDIOC_QBUF failed while queueing capture buffer");
            return false;
        }
    }
    return true;
}

bool V4L2CaptureDevice::StartStreaming() {
    // 真正开始视频流采集。
    enum v4l2_buf_type buffer_type = V4L2_BUF_TYPE_VIDEO_CAPTURE;

    // VIDIOC_STREAMON 的作用是通知 V4L2 驱动： 可以开始从摄像头采集数据，并写入已经入队的 buffer
    if (IoctlWithRetry(VIDIOC_STREAMON, &buffer_type) < 0) {
        common::log::Logger::Error("VIDIOC_STREAMON failed");
        return false;
    }
    return true;
}

void V4L2CaptureDevice::StopStreaming() {
    // 停止视频流采集。
    enum v4l2_buf_type buffer_type = V4L2_BUF_TYPE_VIDEO_CAPTURE;

    // VIDIOC_STREAMOFF 的作用是通知 V4L2 驱动：停止摄像头采集，不再继续向 buffer 写入新帧。
    IoctlWithRetry(VIDIOC_STREAMOFF, &buffer_type);
}

void V4L2CaptureDevice::ReleaseMappedBuffers() {
    // 释放 mmap 映射，避免用户态地址空间泄漏。
    for (std::size_t index = 0; index < buffers_.size(); ++index) {
        if (buffers_[index].start != nullptr) {
            // 解除 mmap 映射
            munmap(buffers_[index].start, buffers_[index].length);
            buffers_[index].start = nullptr;
            buffers_[index].length = 0;
        }
    }
    buffers_.clear();
}

}  // namespace v4l2
}  // namespace capture
}  // namespace modules
}  // namespace sserver
