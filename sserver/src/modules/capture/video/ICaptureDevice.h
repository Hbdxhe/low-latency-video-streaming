#ifndef SSERVER_MODULES_CAPTURE_VIDEO_ICAPTUREDEVICE_H
#define SSERVER_MODULES_CAPTURE_VIDEO_ICAPTUREDEVICE_H

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "common/model/EncodedFrame.h"

namespace sserver {
namespace modules {
namespace capture {

// 原始采集帧。
//
// 这是“尚未编码”的视频数据，典型来源是 V4L2 摄像头输出的 YUYV422。
// 它位于 CaptureModule 的双线程路径中：
//
//   CapturePump 线程 -> CaptureRawFrame() -> RawCaptureFrame -> raw_queue_
//   EncodePump 线程  -> EncodeRawFrame(raw) -> EncodedFrame
//
// 这样采集和编码可以解耦：摄像头取帧不必等待 x264 编码完成。
struct RawCaptureFrame {
    // 原始像素数据。V4L2 当前配置下通常是 YUYV422，每个像素平均 2 字节。
    std::vector<std::uint8_t> data;
    // 采集时间戳，单位纳秒。优先使用 V4L2 monotonic buffer timestamp。
    std::uint64_t capture_timestamp_ns = 0;
    // data 中实际有效字节数。mmap buffer 长度可能大于本帧实际使用长度。
    std::size_t bytes_used = 0;
};

using RawCaptureFramePtr = std::shared_ptr<RawCaptureFrame>;

// 采集设备统一接口。
//
// Capture 不直接依赖 V4L2CaptureDevice 或 NullCaptureDevice 的具体类型，
// 而是通过 ICaptureDevice 调用统一方法。这样可以在真实摄像头和测试源之间切换。
class ICaptureDevice {
public:
    // 基类析构必须是 virtual，确保通过 ICaptureDevice 指针删除派生类时能正确析构。
    virtual ~ICaptureDevice() = default;

    // 打开设备或测试源。V4L2 会 open(/dev/videoX)，Null 源只是标记 opened。
    virtual bool Open() = 0;
    // 开始采集。V4L2 会配置格式、申请 mmap buffer、启动 streaming、初始化编码器。
    virtual bool Start() = 0;
    // 单线程路径：直接返回编码后的帧。
    virtual common::model::EncodedFramePtr CaptureFrame() = 0;
    // 停止采集。应停止 streaming、关闭编码器等活动工作。
    virtual void Stop() = 0;
    // 关闭设备句柄。V4L2 对应 close(fd)。
    virtual void Close() = 0;
    // 返回可读描述，用于启动日志。
    virtual std::string Describe() const = 0;

    // 是否支持双线程 raw capture 路径。默认 false，表示走 CaptureFrame() 单线程路径。
    virtual bool SupportsRawCapture() const { return false; }
    // 双线程路径的采集阶段：只取原始帧，不编码。
    virtual RawCaptureFramePtr CaptureRawFrame() { return nullptr; }
    // 双线程路径的编码阶段：把原始帧编码成 EncodedFrame。
    virtual common::model::EncodedFramePtr EncodeRawFrame(RawCaptureFramePtr /* raw */) { return nullptr; }
};

}  // namespace capture
}  // namespace modules
}  // namespace sserver

#endif  // SSERVER_MODULES_CAPTURE_VIDEO_ICAPTUREDEVICE_H
