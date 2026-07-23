#ifndef SSERVER_COMMON_MODEL_ENCODEDFRAME_H
#define SSERVER_COMMON_MODEL_ENCODEDFRAME_H

#include <cstdint>
#include <memory>
#include <vector>

namespace sserver {
namespace common {
namespace model {

// 编码载荷的媒体类型。
//
// 指定底层类型为 std::uint16_t，能够明确枚举在内存和协议字段中的宽度，
// 避免不同编译器使用不同大小的 int。枚举值 3、4 属于项目协议的一部分，
// 发送端和接收端必须使用相同定义，不能把它们当成普通序号随意修改。
//
// 当前服务端主要产生 H.264 视频帧；kAudio 是为后续 AAC、Opus 等音频
// 编码链路预留的类型。枚举只说明“媒体种类”，并不直接说明具体编码格式。
enum class StreamPayloadType : std::uint16_t {
    // 视频编码载荷。当前项目中通常表示 H.264 Annex-B 字节流。
    kVideo = 3,

    // 音频编码载荷预留值。当前服务端主链路尚未生产音频帧。
    kAudio = 4,
};

// 服务端模块之间传递的一帧“已编码媒体数据”。
//
// 它是采集/编码模块与传输模块之间的统一数据模型：
//
//   V4L2 摄像头或 Null 模拟源
//              |
//              v
//       原始 YUYV/YUV 视频帧
//              |
//              v
//          x264 编码器
//              |
//              v
//         EncodedFrame
//              |
//              +----> TCP 后端
//              +----> UDP 私有协议后端
//              `----> RTP 后端

struct EncodedFrame {
    // 当前载荷属于视频还是音频。
    StreamPayloadType type = StreamPayloadType::kVideo;

    // 帧序号：在一条媒体流中标识当前编码帧。
    std::uint64_t sequence = 0;

    // 原始画面进入服务端采集链路的时间戳，单位为纳秒。
    std::uint64_t capture_timestamp_ns = 0;

    // 编码器开始处理这一帧的单调时钟时间戳，单位为纳秒。
    std::uint64_t encode_start_timestamp_ns = 0;

    // 编码器完成这一帧的单调时钟时间戳，单位为纳秒。
    std::uint64_t encode_end_timestamp_ns = 0;

    // 当前视频帧是否为关键帧。
    //
    // 在 H.264 链路中，关键帧通常能够作为新的解码起点，不依赖之前的普通
    // 帧即可恢复画面。发生发送队列拥塞时，传输层会优先丢弃非关键帧，尽量
    // 保留关键帧，避免客户端长时间无法重新建立正确的参考图像。
    //
    // 对音频或不使用帧间预测的编码格式，该字段可能没有相同语义。
    bool is_keyframe = false;

    // 已编码媒体载荷的连续字节。
    //
    // 不同传输后端对它的处理方式不同：
    //   TCP：在载荷前添加项目协议头后按字节流发送；
    //   UDP：按照 MTU 拆成多个私有协议分片，并可附加 FEC/NACK 信息；
    //   RTP：解析 H.264 NAL 单元，必要时使用 FU-A 分片封装。
    //
    // vector 自己管理内存，EncodedFrame 销毁时载荷会自动释放。
    std::vector<std::uint8_t> payload;
};

// EncodedFrame 的共享只读智能指针。
//
// shared_ptr：
//   同一帧可能同时发送给多个 TCP 客户端，或被传输队列、统计模块暂时持有。
//   使用引用计数可以让这些使用者共享 payload，避免为每个会话复制一份可能
//   很大的 H.264 数据；最后一个持有者释放指针时，帧会自动销毁。
//
// const EncodedFrame：
//   帧发布给下游后视为不可变对象。传输模块只能读取字段，不能修改 sequence、
//   时间戳或 payload，从而避免多个线程共享一帧时发生数据竞争或内容不一致。
//
using EncodedFramePtr = std::shared_ptr<const EncodedFrame>;

}  // namespace model
}  // namespace common
}  // namespace sserver

#endif  // SSERVER_COMMON_MODEL_ENCODEDFRAME_H
