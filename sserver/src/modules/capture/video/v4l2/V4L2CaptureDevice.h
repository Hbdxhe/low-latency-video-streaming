#ifndef SSERVER_MODULES_CAPTURE_VIDEO_V4L2_V4L2CAPTUREDEVICE_H
#define SSERVER_MODULES_CAPTURE_VIDEO_V4L2_V4L2CAPTUREDEVICE_H

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include <linux/videodev2.h>

#include "config/AppConfig.h"
#include "modules/capture/video/ICaptureDevice.h"
#include "modules/encoding/video/VideoEncoder.h"

namespace sserver {
namespace modules {
namespace capture {
namespace v4l2 {

// 通过 mmap 映射到用户态的 V4L2 内核缓冲区。
struct MappedBuffer {
    // 映射起始地址。
    void *start = nullptr;
    // 映射长度。
    std::size_t length = 0;
};

// Linux V4L2 真实摄像头实现。
//
// 主要流程：
//   Open -> 配置格式/帧率 -> mmap buffers -> STREAMON
//   -> DQBUF 取 YUYV422 -> 编码 H.264 -> QBUF 归还
//   -> STREAMOFF -> munmap -> Close
class V4L2CaptureDevice : public ICaptureDevice {
public:
    V4L2CaptureDevice(const config::CaptureConfig &capture_config, const config::CodecConfig &codec_config);
    ~V4L2CaptureDevice() override;

    // 打开 /dev/videoX。
    bool Open() override;
    // 配置设备、申请缓冲区、初始化编码器并开始采集。
    bool Start() override;
    // 兼容单线程路径：采集并编码一帧。
    common::model::EncodedFramePtr CaptureFrame() override;
    // 停止 streaming、编码器和 mmap。
    void Stop() override;
    // 关闭设备 fd。
    void Close() override;
    std::string Describe() const override;

    // V4L2 支持采集/编码双线程拆分。
    bool SupportsRawCapture() const override;
    // 只采集一帧原始 YUYV422。
    RawCaptureFramePtr CaptureRawFrame() override;
    // 编码一帧已复制的 YUYV422。
    common::model::EncodedFramePtr EncodeRawFrame(RawCaptureFramePtr raw) override;

private:
    // ioctl 遇到 EINTR 时自动重试。
    int IoctlWithRetry(unsigned long request, void *arg);
    // 检查视频采集和 streaming 能力。
    bool QueryCapabilities();
    // 设置宽高与 YUYV422 格式。
    bool ConfigureFormat();
    // 尝试设置 fps。
    bool ConfigureFrameRate();
    // 申请内核缓冲区并 mmap。
    bool InitializeMemoryMapping();
    // 将缓冲区通过 QBUF 交给驱动。
    bool QueueCaptureBuffers();
    // VIDIOC_STREAMON。
    bool StartStreaming();
    // VIDIOC_STREAMOFF。
    void StopStreaming();
    // munmap 所有映射。
    void ReleaseMappedBuffers();

private:
    // 摄像头采集配置。
    config::CaptureConfig capture_config_;
    // x264 编码配置。
    config::CodecConfig codec_config_;
    // YUYV422 -> H.264 编码器。
    std::unique_ptr<modules::encoding::VideoEncoder> encoder_;
    // /dev/videoX 文件描述符。
    int device_fd_;
    bool opened_;
    bool streaming_;
    // 控制时间戳来源日志只输出一次。
    bool logged_timestamp_source_;
    // mmap 后的缓冲区列表。
    std::vector<MappedBuffer> buffers_;
    // 输出帧序号。
    std::uint64_t sequence_;
};

}  // namespace v4l2
}  // namespace capture
}  // namespace modules
}  // namespace sserver

#endif  // SSERVER_MODULES_CAPTURE_VIDEO_V4L2_V4L2CAPTUREDEVICE_H
