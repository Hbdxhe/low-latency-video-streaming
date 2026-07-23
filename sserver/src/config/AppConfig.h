#ifndef SSERVER_CONFIG_APPCONFIG_H
#define SSERVER_CONFIG_APPCONFIG_H

#include <cstddef>
#include <string>

namespace sserver {
namespace config {

// 运行时配置：控制服务端进程级行为，不直接决定视频内容或传输协议。
struct RuntimeConfig {
    // 收到停止请求后，模块最多允许的优雅退出等待时间。
    // 典型用途：给后台线程、socket、摄像头释放留一点收尾时间。
    int shutdown_grace_period_ms = 500;

    // 每隔多少帧打印一次延迟统计。
    // 值越小，日志越密；值越大，日志越安静。
    int latency_log_interval_frames = 120;
};

// 采集配置：决定服务端从哪里拿原始视频帧。
//
// 当前支持两类源：
//   v4l2：真实 Linux 摄像头，例如 /dev/video0
//   null：测试源，用于没有摄像头时生成测试帧或测试码流
struct CaptureConfig {
    // 是否启用采集模块。正常视频服务应为 true。
    bool enabled = true;

    // 采集源类型。常见值：v4l2 / null。
    std::string source = "v4l2";

    // V4L2 设备路径。真实摄像头通常是 /dev/video0、/dev/video1。
    std::string device = "/dev/video0";

    // 采集分辨率宽度。
    int width = 640;

    // 采集分辨率高度。
    int height = 360;

    // 目标采集帧率。
    int fps = 30;

    // 测试源或节流场景下的帧间隔。
    // 为 0 时通常由 fps 或设备自身节奏控制。
    int frame_interval_ms = 0;

    // V4L2 内核缓冲区数量。
    // 数量越大越不容易丢采集帧，但排队越多也可能增加采集端延迟。
    int device_buffer_count = 2;

    // null 源生成的测试 payload 大小。为 0 时由具体 null 模式决定。
    std::size_t null_payload_bytes = 0;

    // null 源模式：
    //   text              生成文本测试帧，适合协议/传输测试
    //   h264_test_pattern 生成可解码的视频测试图案，适合端到端显示测试
    std::string null_payload_mode = "text";
};

// 传输配置：决定编码后的 EncodedFrame 怎么发出去。
//
// 三类后端：
//   tcp：可靠字节流，容易跑通，但丢包重传可能带来阻塞
//   udp：自定义 UDP 分片/重组，可选 NACK/FEC，低延迟实验重点
//   rtp：标准实时传输协议，适合音视频领域通用互通
struct TransportConfig {
    // 是否启用传输模块。
    bool enabled = true;

    // 传输后端类型：tcp / udp / rtp。
    std::string backend = "tcp";

    // TCP/UDP 绑定地址。127.0.0.1 表示只允许本机访问。
    std::string bind_address = "0.0.0.0";

    // TCP/UDP 监听端口。0 表示让操作系统自动分配端口，常用于测试。
    int listen_port = 9999;

    // 每个客户端发送队列最多缓存多少帧。
    // 低延迟系统通常保持很小，避免旧帧堆积。
    std::size_t max_pending_frames = 3;

    // 帧在发送队列里允许等待的最大时间。
    // 超过后可认为太旧，应按策略丢弃。
    int max_queue_wait_ms = 50;

    // 队列满时的丢帧策略：
    //   drop_oldest         丢最旧帧
    //   drop_oldest_non_key 优先丢最旧的非关键帧，尽量保留关键帧
    std::string queue_drop_policy = "drop_oldest_non_key";

    // TCP accept 循环空转等待时间。值越小连接响应越快，但 CPU 空转可能略多。
    int accept_loop_interval_ms = 5;

    // TCP_NODELAY 开关。true 表示禁用 Nagle 算法，减少小包等待，偏低延迟。
    bool enable_nodelay = true;

    // 是否把采集/编码/发送时间戳嵌入帧数据，供客户端计算端到端延迟。
    // TCP/UDP 通常打开；RTP 使用 header extension，因此这个字段常为 false。
    bool embed_frame_metadata = false;

    // UDP 客户端多久不发 keepalive 就被认为离线。
    int udp_client_timeout_ms = 5000;

    // UDP 单个 datagram 的最大大小。65507 是 IPv4 UDP payload 理论上限。
    std::size_t udp_max_datagram_size = 65507;

    // UDP 每个数据分片的目标 payload 大小。
    // 大包效率高但更怕丢包；小包更利于 FEC/NACK，但包数量更多。
    std::size_t udp_target_payload_size = 65000;

    // UDP socket 接收缓冲区大小。
    int udp_receive_buffer_bytes = 4 * 1024 * 1024;

    // UDP socket 发送缓冲区大小。
    int udp_send_buffer_bytes = 4 * 1024 * 1024;

    // 是否启用 NACK：客户端发现缺片后请求服务端重传。
    bool udp_enable_nack = false;

    // 是否启用 FEC：服务端发送冗余校验分片，客户端可恢复少量丢包。
    bool udp_enable_fec = false;

    // UDP 重传缓存保留最近多少帧，用于响应 NACK。
    std::size_t udp_retransmit_cache_frames = 32;

    // UDP 重传缓存中单帧的最长保留时间。
    int udp_retransmit_cache_max_age_ms = 500;

    // 单个 NACK 请求最多允许重传多少个分片，避免一次请求过大。
    std::size_t udp_retransmit_max_fragments_per_request = 16;

    // RTP 远端地址。RTP 模式是服务端主动向这个地址推流。
    std::string rtp_remote_host = "127.0.0.1";

    // RTP 远端端口，客户端需要先监听这个端口。
    int rtp_remote_port = 5004;

    // RTP payload type。动态类型常用 96 表示 H.264。
    int rtp_payload_type = 96;

    // RTP 时钟频率。视频 RTP/H.264 通常使用 90000 Hz。
    int rtp_clock_rate = 90000;

    // RTP 同步源标识，用于区分不同媒体源。
    int rtp_ssrc = 305419896;

    // RTP 单包最大媒体 payload。超过后 H.264 NALU 会被拆成 FU-A 分片。
    std::size_t rtp_max_payload_size = 1200;

    // 是否启用自定义 RTP 延迟扩展头，用来携带采集/发送时间戳。
    bool rtp_enable_latency_extension = true;

    // 服务端生成的 SDP 文件路径，供 ffplay 或其他 RTP 播放器理解流参数。
    std::string rtp_sdp_path = "sserver_rtp.sdp";
};

// 编码配置：决定原始 YUV 视频如何压缩成 H.264。
//
// 当前主要后端是 x264。这里的默认值偏实时低延迟，而不是极致压缩率。
struct CodecConfig {
    // 编码器后端，目前主要是 x264。
    std::string backend = "x264";

    // x264 编码速度/压缩率预设。ultrafast 编码最快，码率相对更高。
    std::string x264_preset = "ultrafast";

    // x264 调优方向。zerolatency 会关闭一些增加延迟的缓存/前瞻机制。
    std::string x264_tune = "zerolatency";

    // H.264 profile。baseline 兼容性好，常用于低复杂度实时场景。
    std::string x264_profile = "baseline";

    // 是否输出 Annex-B 格式。实时传输里常用起始码分隔 NALU。
    bool x264_annexb = true;

    // 是否重复发送 SPS/PPS。打开后客户端中途加入更容易等到解码参数。
    bool x264_repeat_headers = true;

    // 最大关键帧间隔。值越小，客户端等待关键帧时间越短，但码率会上升。
    int x264_keyint_max = 30;

    // 最小关键帧间隔。
    int x264_keyint_min = 30;

    // B 帧数量。低延迟场景通常设为 0，因为 B 帧会引入重排序延迟。
    int x264_bframes = 0;

    // x264 编码线程数。实时低延迟常用较少线程，减少排队和调度复杂度。
    int x264_threads = 1;

    // sliced threads 允许按 slice 并行，兼顾并行和低延迟。
    bool x264_sliced_threads = true;

    // 固定 slice 数量。
    int x264_slice_count = 1;

    // 最大 slice 数量。
    int x264_slice_count_max = 1;

    // 参考帧数量。低延迟通常保持较小。
    int x264_frame_reference = 1;

    // 前瞻帧数。低延迟通常为 0。
    int x264_lookahead = 0;

    // 同步前瞻帧数。低延迟通常为 0。
    int x264_sync_lookahead = 0;

    // 亚像素搜索精度。越高画质可能更好，但编码更慢。
    int x264_subpel_refine = 0;

    // 宏块树码率控制。低延迟配置中通常关闭。
    bool x264_mb_tree = false;

    // 场景切换检测。低延迟固定 GOP 场景中通常关闭。
    int x264_scenecut = 0;
};

// 服务端总配置。main.cpp 只加载一个 AppConfig，后续通过 ApplicationContext
// 分发给采集、编码、传输等模块。
struct AppConfig {
    std::string app_name = "sserver";
    RuntimeConfig runtime;
    CaptureConfig capture;
    TransportConfig transport;
    CodecConfig codec;

    // 返回默认配置对象。LoadFromFile 会先创建默认配置，再用文件里的 key 覆盖。
    static AppConfig CreateDefault();

    // 校验配置组合是否合法。解析成功不代表配置合理，因此启动前必须 Validate。
    bool Validate(std::string *error_message) const;
};

// 简单的 key=value 配置加载器。
//
// 支持：
//   # 注释行
//   空行
//   key = value
//
// 不支持嵌套、不支持 include、不支持行内注释。
class ConfigLoader {
public:
    static bool LoadFromFile(const std::string &file_path, AppConfig *config, std::string *error_message);
};

}  // namespace config
}  // namespace sserver

#endif  // SSERVER_CONFIG_APPCONFIG_H
