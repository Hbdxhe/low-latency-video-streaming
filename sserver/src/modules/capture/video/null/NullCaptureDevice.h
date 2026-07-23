#ifndef SSERVER_MODULES_CAPTURE_VIDEO_NULL_NULLCAPTUREDEVICE_H
#define SSERVER_MODULES_CAPTURE_VIDEO_NULL_NULLCAPTUREDEVICE_H

#include <cstdint>
#include <memory>
#include <vector>

#include "config/AppConfig.h"
#include "modules/capture/video/ICaptureDevice.h"
#include "modules/encoding/video/VideoEncoder.h"

namespace sserver {
namespace modules {
namespace capture {

// 无硬件测试采集源。
//
// text 模式生成文本 payload，用于协议测试；
// h264_test_pattern 模式生成 YUYV422 图案并编码成 H.264，可跑完整显示链路。
// Null 后端不支持 raw 双线程接口，因此走 CaptureModule::CaptureLoop()。
class NullCaptureDevice : public ICaptureDevice {
public:
    NullCaptureDevice(const config::CaptureConfig &config, const config::CodecConfig &codec_config);

    // 无真实 fd，Open/Close 只维护状态。
    bool Open() override;
    // 按模式准备编码器和合成帧缓冲区。
    bool Start() override;
    // 生成一帧文本数据或可解码 H.264。
    common::model::EncodedFramePtr CaptureFrame() override;
    // 停止产帧并释放编码器。
    void Stop() override;
    void Close() override;
    std::string Describe() const override;

private:
    // 生成随帧序号变化的 YUYV422 测试图案。
    void FillSyntheticYuyvFrame();

    // 采集参数：宽高、帧间隔、null 模式等。
    config::CaptureConfig config_;
    // h264_test_pattern 使用的 x264 参数。
    config::CodecConfig codec_config_;
    bool opened_;
    bool streaming_;
    // 递增帧序号。
    std::uint64_t sequence_;
    // 测试图案模式使用的软件编码器。
    std::unique_ptr<modules::encoding::VideoEncoder> encoder_;
    // YUYV422 合成帧缓冲区，大小通常为 width * height * 2。
    std::vector<std::uint8_t> synthetic_yuyv_frame_;
};

}  // namespace capture
}  // namespace modules
}  // namespace sserver

#endif  // SSERVER_MODULES_CAPTURE_VIDEO_NULL_NULLCAPTUREDEVICE_H
