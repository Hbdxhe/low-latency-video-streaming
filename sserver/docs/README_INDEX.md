# 文档索引

## 所有 README.md 列表

| 路径 | 模块说明 |
|---|---|
| [docs/src_architecture_overview.md](src_architecture_overview.md) | 项目整体架构总览，包含目录树、数据链路、模块依赖、新人阅读指南 |
| [src/app/README.md](../src/app/README.md) | 程序入口与启动编排，负责配置加载、模块注册、信号处理、数据流绑定 |
| [src/core/README.md](../src/core/README.md) | 应用生命周期与模块编排，定义 IModule 接口和模块状态管理 |
| [src/config/README.md](../src/config/README.md) | 配置模型与加载校验，定义所有可配置参数和 .conf 文件解析 |
| [src/common/README.md](../src/common/README.md) | 公共基础能力总览，包含日志、队列、数据模型、协议结构、时间工具 |
| [src/common/log/README.md](../src/common/log/README.md) | 日志系统，线程安全的 Debug/Info/Warn/Error 输出 |
| [src/common/metrics/README.md](../src/common/metrics/README.md) | 延迟统计，纳秒级延迟记录和百分位计算 |
| [src/common/concurrency/README.md](../src/common/concurrency/README.md) | 并发工具，ThreadSafeQueue 模板（阻塞等待、丢弃最旧、选择性丢弃） |
| [src/common/model/README.md](../src/common/model/README.md) | 公共数据模型，EncodedFrame 结构体定义 |
| [src/common/net/README.md](../src/common/net/README.md) | 网络协议结构，TCP/UDP 协议头、RTP 协议工具、H.264 Annex-B 工具 |
| [src/common/time/README.md](../src/common/time/README.md) | 时间工具，MonotonicNowNs() 单调时钟 |
| [src/modules/capture/README.md](../src/modules/capture/README.md) | 视频采集模块，V4L2/Null 设备、采集线程、编码集成 |
| [src/modules/encoding/README.md](../src/modules/encoding/README.md) | 视频编码模块，x264 软件编码、YUYV→I420 色彩转换（NEON 加速） |
| [src/modules/transport/README.md](../src/modules/transport/README.md) | 流传输模块，TCP/UDP/RTP 三种后端、分片、FEC、NACK、Pacing |
| [tests/README.md](../tests/README.md) | 测试总览，测试分层、配置文件、运行方式、CTest 注册 |
| [tests/smoke/README.md](../tests/smoke/README.md) | 烟雾测试，验证配置加载和模块生命周期 |
| [tests/integration/README.md](../tests/integration/README.md) | 集成测试，验证端到端链路、协议校验、NACK/FEC 恢复 |
| [tests/benchmark/README.md](../tests/benchmark/README.md) | 传输基准测试，分阶段延迟统计 |
| [tests/latency/README.md](../tests/latency/README.md) | 端到端延迟基准，FFmpeg 解码 + OpenCV 渲染 |
| [tests/unit/README.md](../tests/unit/README.md) | 单元测试，协议解析、配置校验、模块回滚、延迟统计 |
| [tests/support/README.md](../tests/support/README.md) | 测试辅助工具，TCP/UDP 帧接收、分片重组、FEC 恢复 |
| [config/README.md](../config/README.md) | 配置文件目录，25 个 .conf 文件分类说明、关键配置项差异对比 |
| [scripts/README.md](../scripts/README.md) | 构建/测试/运行脚本，build.sh、test.sh、tcp.sh、udp.sh、rtp.sh |

## 推荐阅读顺序

### 新手入门（从宏观到微观）

1. [docs/src_architecture_overview.md](src_architecture_overview.md) — 先看整体架构，理解项目全貌
2. [src/config/README.md](../src/config/README.md) — 理解所有可配置参数
3. [src/app/README.md](../src/app/README.md) — 理解程序启动流程
4. [src/core/README.md](../src/core/README.md) — 理解模块生命周期框架
5. [src/common/model/README.md](../src/common/model/README.md) — 理解核心数据结构 EncodedFrame
6. [src/modules/capture/README.md](../src/modules/capture/README.md) — 理解采集链路
7. [src/modules/encoding/README.md](../src/modules/encoding/README.md) — 理解编码链路
8. [src/modules/transport/README.md](../src/modules/transport/README.md) — 理解传输链路

### 调试性能问题

1. [src/common/metrics/README.md](../src/common/metrics/README.md) — 理解延迟统计机制
2. [src/modules/capture/README.md](../src/modules/capture/README.md) — 检查采集线程模型和丢帧策略
3. [src/modules/encoding/README.md](../src/modules/encoding/README.md) — 检查编码参数和 NEON 加速
4. [src/modules/transport/README.md](../src/modules/transport/README.md) — 检查队列深度、背压控制、Pacing 策略

### 理解网络传输

1. [src/common/net/README.md](../src/common/net/README.md) — 理解协议格式（TCP/UDP/RTP）
2. [src/modules/transport/README.md](../src/modules/transport/README.md) — 理解三种传输后端的实现细节

### 理解音视频链路

1. [src/common/model/README.md](../src/common/model/README.md) — EncodedFrame 数据结构
2. [src/common/net/H264AnnexB.h](../src/common/net/README.md) — H.264 Annex-B NALU 工具
3. [src/modules/capture/README.md](../src/modules/capture/README.md) — 采集 → 编码流程
4. [src/modules/encoding/README.md](../src/modules/encoding/README.md) — YUYV422 → H.264 编码流程
5. [src/modules/transport/README.md](../src/modules/transport/README.md) — RTP 打包和 FU-A 分片

### 理解测试体系

1. [tests/README.md](../tests/README.md) — 测试分层、运行方式、CTest 配置
2. [tests/support/README.md](../tests/support/README.md) — 测试辅助工具（帧接收、分片重组、FEC 恢复）
3. [tests/unit/README.md](../tests/unit/README.md) — 单元测试详情
4. [tests/integration/README.md](../tests/integration/README.md) — 集成测试详情
5. [tests/benchmark/README.md](../tests/benchmark/README.md) — 传输基准测试
6. [tests/latency/README.md](../tests/latency/README.md) — 端到端延迟基准测试

### 快速上手（构建、运行、调试）

1. [scripts/README.md](../scripts/README.md) — 构建和运行脚本
2. [config/README.md](../config/README.md) — 配置文件说明和分类
3. [docs/start.md](start.md) — 快速启动指南
