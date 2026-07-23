#include "modules/capture/video/CaptureModule.h"

#include <chrono>

#include "common/log/Logger.h"

namespace sserver {
namespace modules {
namespace capture {

CaptureModule::CaptureModule()
        // Capture 是采集子系统的门面对象，内部会根据配置选择 V4L2 或 Null 设备。
        : capture_(std::make_unique<Capture>()),
          running_(false),
          state_(core::ModuleState::kCreated),
          use_raw_capture_(false) {
}

CaptureModule::~CaptureModule() {
    shutdown();
}

std::string CaptureModule::name() const {
    return "CaptureModule";
}

bool CaptureModule::initialize(const core::ApplicationContext &context) {
    // initialize 阶段只做后端选择和对象准备，不启动采集线程。
    // 真实打开摄像头、启动 V4L2 streaming 会发生在 start()。
    std::string error_message;
    if (!capture_->initialize(context, &error_message)) {
        if (!error_message.empty()) {
            common::log::Logger::Error("failed to initialize capture: " + error_message);
        }
        state_ = core::ModuleState::kFailed;
        return false;
    }
    state_ = core::ModuleState::kInitialized;
    return true;
}

bool CaptureModule::start() {
    // 启动底层采集设备。
    // 对 V4L2 来说，这一步会打开/配置摄像头、初始化 x264 编码器并开始 streaming。
    std::string error_message;
    if (!capture_->start(&error_message)) {
        if (!error_message.empty()) {
            common::log::Logger::Error("failed to start capture: " + error_message);
        }
        state_ = core::ModuleState::kFailed;
        return false;
    }

    running_ = true;
    use_raw_capture_ = capture_->SupportsRawCapture();
    if (use_raw_capture_) {
        // 双线程低延迟路径：
        //   worker_thread_ 负责尽快从摄像头取出原始帧
        //   encode_thread_ 负责从 raw_queue_ 取帧并编码
        //
        // 好处：摄像头采集不被 x264 编码耗时直接阻塞。
        worker_thread_ = std::thread(&CaptureModule::CapturePump, this);
        encode_thread_ = std::thread(&CaptureModule::EncodePump, this);
    } else {
        // 单线程路径：CaptureFrame() 内部完成“采集 + 编码”。
        // NullCaptureDevice 当前走这条路径。
        worker_thread_ = std::thread(&CaptureModule::CaptureLoop, this);
    }
    state_ = core::ModuleState::kRunning;
    common::log::Logger::Info("capture module started with " + capture_->Describe() +
                              (use_raw_capture_ ? " (dual-thread raw capture)" : " (single-thread)"));
    return true;
}

void CaptureModule::stop() {
    if (state_.load() != core::ModuleState::kRunning) {
        return;
    }

    // 先清 running_，让循环自然退出；再唤醒可能阻塞在 WaitPopFor() 的编码线程。
    running_ = false;
    raw_queue_.NotifyAll();

    // 等待采集线程退出。
    if (worker_thread_.joinable()) {
        worker_thread_.join();
    }
    // 等待编码线程退出。单线程路径下 encode_thread_ 不会 joinable。
    if (encode_thread_.joinable()) {
        encode_thread_.join();
    }
    capture_->stop();
    state_ = core::ModuleState::kStopped;
}

void CaptureModule::shutdown() {
    stop();
    capture_->shutdown();
    state_ = core::ModuleState::kShutdown;
}

core::ModuleState CaptureModule::state() const {
    return state_.load();
}

void CaptureModule::SetFrameHandler(FrameHandler handler) {
    // AppBootstrap 会用这个接口把 CaptureModule 的输出接到 TransportModule。
    // 加锁是因为回调可能在主线程设置，而采集/编码线程同时读取。
    std::lock_guard<std::mutex> lock(handler_mutex_);
    frame_handler_ = handler;
}

// 单线程路径：采集与编码在同一线程中完成（适用于不支持原始帧采集的设备）
void CaptureModule::CaptureLoop() {
    while (running_.load()) {
        common::model::EncodedFramePtr frame = capture_->CaptureFrame();
        if (!frame) {
            std::this_thread::sleep_for(std::chrono::microseconds(100));
            continue;
        }

        // 复制一份回调到局部变量，然后释放锁再调用。
        // 这样避免 TransportModule::Broadcast() 执行期间长时间持有 handler_mutex_。
        FrameHandler handler_copy;
        {
            std::lock_guard<std::mutex> lock(handler_mutex_);
            handler_copy = frame_handler_;
        }
        if (handler_copy) {
            handler_copy(frame);
        }
    }
}

// 采集泵：从设备取出原始帧 → 拷贝 → 归还缓冲区 → 入队等待编码
void CaptureModule::CapturePump() {
    while (running_.load()) {
        RawCaptureFramePtr raw = capture_->CaptureRawFrame();
        if (!raw) {
            std::this_thread::sleep_for(std::chrono::microseconds(100));
            continue;
        }
        // raw_queue_ 只保留少量帧，满了丢最旧帧。
        // 低延迟系统宁可丢旧帧，也不希望排队导致画面越来越滞后。
        raw_queue_.PushDropOldest(raw, 3);
    }
    raw_queue_.NotifyAll();
}

// 编码泵：从队列取出原始帧 → 编码 → 分发给下游
void CaptureModule::EncodePump() {
    while (running_.load()) {
        RawCaptureFramePtr raw;
        if (!raw_queue_.WaitPopFor(&raw, std::chrono::milliseconds(10))) {
            continue;
        }
        if (!raw) {
            continue;
        }

        // EncodeRawFrame() 会把 YUYV422 原始数据转换成 I420，并调用 x264 生成 H.264。
        common::model::EncodedFramePtr frame = capture_->EncodeRawFrame(raw);
        if (!frame) {
            continue;
        }

        // 和单线程路径一样，复制回调后释放锁再执行，为了避免长时间持有锁
        FrameHandler handler_copy;
        {
            std::lock_guard<std::mutex> lock(handler_mutex_);
            handler_copy = frame_handler_;
        }
        if (handler_copy) {
            handler_copy(frame);
        }
    }
}

}  // namespace capture
}  // namespace modules
}  // namespace sserver
