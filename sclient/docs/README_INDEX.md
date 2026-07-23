# 文档索引

本文档是 sclient 项目源码架构文档的索引，帮助开发者快速找到需要的文档。

## 文档列表

### 总览文档

| 路径 | 说明 |
|---|---|
| [docs/src_architecture_overview.md](src_architecture_overview.md) | 项目整体架构、目录树、数据链路、模块依赖、推荐阅读顺序 |

### src/ 子目录文档

| 路径 | 模块说明 |
|---|---|
| [src/app/README.md](../src/app/README.md) | 应用入口层：程序入口、CLI 解析、管线编排、三线程模型 |
| [src/app/cli/README.md](../src/app/cli/README.md) | CLI 参数解析子模块 |
| [src/adapters/README.md](../src/adapters/README.md) | 适配器层（当前为空，预留扩展） |
| [src/common/README.md](../src/common/README.md) | 公共基础层总览：日志、协议、并发、度量、网络工具 |
| [src/common/concurrency/README.md](../src/common/concurrency/README.md) | 并发工具：BoundedQueue 有界线程安全队列 |
| [src/common/log/README.md](../src/common/log/README.md) | 日志模块：Logger 类，控制台 + 文件双输出 |
| [src/common/media/README.md](../src/common/media/README.md) | 媒体数据结构：DecodedFrame 解码帧结构体 |
| [src/common/metrics/README.md](../src/common/metrics/README.md) | 度量工具：LatencyStats 延迟统计（环形缓冲、百分位） |
| [src/common/net/README.md](../src/common/net/README.md) | 网络工具：H264 Annex B、RTP 协议、SDP 解析 |
| [src/common/protocol/README.md](../src/common/protocol/README.md) | 应用层协议定义：消息头、UDP 分片、NACK、接收报告 |
| [src/modules/README.md](../src/modules/README.md) | 业务模块层总览：网络、解码、渲染 |
| [src/modules/network/README.md](../src/modules/network/README.md) | 网络接收模块：TCP/UDP/RTP 接收、分片重组、NACK/FEC、jitter buffer |
| [src/modules/network/types/README.md](../src/modules/network/types/README.md) | 网络模块数据类型：ClientConfig、ReceivedFrame、UdpReceiveStats |
| [src/modules/decoding/README.md](../src/modules/decoding/README.md) | 视频解码模块：H.264 解码，统一接口 + 后端工厂 |
| [src/modules/decoding/software/README.md](../src/modules/decoding/software/README.md) | FFmpeg 软件解码后端实现 |
| [src/modules/rendering/README.md](../src/modules/rendering/README.md) | 视频渲染模块：OpenGL 渲染、HUD 叠加 |
| [src/modules/rendering/opengl/README.md](../src/modules/rendering/opengl/README.md) | OpenGL 渲染后端实现：窗口、纹理、着色器、ImGui HUD |

### scripts/ 文档

| 路径 | 说明 |
|---|---|
| [scripts/README.md](../scripts/README.md) | 启动脚本：TCP/UDP/RTP 联调快捷启动，三档 UDP 配置 |

### tests/ 子目录文档

| 路径 | 模块说明 |
|---|---|
| [tests/README.md](../tests/README.md) | 测试目录总览：分层说明、运行方式 |
| [tests/unit/README.md](../tests/unit/README.md) | 单元测试：CLI、队列、延迟统计、RTP 协议、SDP 解析 |
| [tests/integration/README.md](../tests/integration/README.md) | 集成测试：TCP/UDP/RTP loopback 端到端验证 |
| [tests/smoke/README.md](../tests/smoke/README.md) | 冒烟测试：最小 RTP 接收 + 解码链路验证 |
| [tests/benchmark/README.md](../tests/benchmark/README.md) | 基准测试：UDP 抖动、延迟统计观测工具 |
| [tests/support/README.md](../tests/support/README.md) | 测试支撑工具：断言、socket、H.264 样本、时钟 |

## 推荐阅读顺序

### 新手入门

1. [docs/src_architecture_overview.md](src_architecture_overview.md) — 先看总览，理解整体架构
2. [src/app/README.md](../src/app/README.md) — 理解程序入口和管线编排
3. [src/common/protocol/README.md](../src/common/protocol/README.md) — 理解协议格式
4. [src/modules/network/README.md](../src/modules/network/README.md) — 理解网络接收
5. [src/modules/decoding/README.md](../src/modules/decoding/README.md) — 理解解码流程
6. [src/modules/rendering/README.md](../src/modules/rendering/README.md) — 理解渲染流程

### 调试网络问题

1. [src/modules/network/README.md](../src/modules/network/README.md) — 网络模块总览
2. [src/common/protocol/README.md](../src/common/protocol/README.md) — 协议格式
3. [src/modules/network/types/README.md](../src/modules/network/types/README.md) — 配置参数和统计结构
4. [src/common/net/README.md](../src/common/net/README.md) — RTP/SDP 工具

### 调试解码问题

1. [src/modules/decoding/README.md](../src/modules/decoding/README.md) — 解码模块总览
2. [src/modules/decoding/software/README.md](../src/modules/decoding/software/README.md) — FFmpeg 解码实现
3. [src/common/media/README.md](../src/common/media/README.md) — 解码帧数据结构

### 调试渲染问题

1. [src/modules/rendering/README.md](../src/modules/rendering/README.md) — 渲染模块总览
2. [src/modules/rendering/opengl/README.md](../src/modules/rendering/opengl/README.md) — OpenGL 实现细节

### 调试性能/延迟问题

1. [src/common/metrics/README.md](../src/common/metrics/README.md) — 延迟统计工具
2. [src/app/README.md](../src/app/README.md) — 管线线程模型和队列配置
3. [src/common/concurrency/README.md](../src/common/concurrency/README.md) — 队列机制
4. [src/modules/network/README.md](../src/modules/network/README.md) — jitter buffer 和丢包恢复

### 理解 UDP 传输细节

1. [src/modules/network/README.md](../src/modules/network/README.md) — UDP 分片重组、NACK、FEC
2. [src/common/protocol/README.md](../src/common/protocol/README.md) — UDP 分片头格式
3. [src/modules/network/types/README.md](../src/modules/network/types/README.md) — UDP 配置参数

### 理解 RTP 传输细节

1. [src/modules/network/README.md](../src/modules/network/README.md) — RTP 接收和 FU-A 重组
2. [src/common/net/README.md](../src/common/net/README.md) — RTP 协议解析和 SDP

### 理解音视频链路

1. [docs/src_architecture_overview.md](src_architecture_overview.md) — 从采集到渲染的完整流程
2. [src/common/net/README.md](../src/common/net/README.md) — H.264 Annex B 工具
3. [src/modules/decoding/README.md](../src/modules/decoding/README.md) — H.264 解码
4. [src/modules/rendering/opengl/README.md](../src/modules/rendering/opengl/README.md) — YUV→RGB 渲染

### 理解测试体系

1. [tests/README.md](../tests/README.md) — 测试分层总览
2. [tests/unit/README.md](../tests/unit/README.md) — 单元测试详情
3. [tests/integration/README.md](../tests/integration/README.md) — 集成测试详情
4. [tests/smoke/README.md](../tests/smoke/README.md) — 冒烟测试详情
5. [tests/benchmark/README.md](../tests/benchmark/README.md) — 基准测试详情
6. [tests/README.md](../tests/README.md) — 测试运行指南
