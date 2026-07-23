# tests

## 1. 模块定位

测试目录。包含项目的全部测试代码，按分层组织：单元测试、集成测试、冒烟测试和基准测试。测试不依赖任何外部测试框架，使用自定义的轻量断言宏。

## 2. 核心职责

- **单元测试**（`unit/`）：验证 CLI 解析、协议处理、SDP 解析、并发队列、延迟统计等纯逻辑
- **集成测试**（`integration/`）：验证本地 loopback 的 TCP/UDP/RTP 接收链路
- **冒烟测试**（`smoke/`）：验证最小 RTP 接收 + 解码链路（进程内自测）
- **基准测试**（`benchmark/`）：UDP 接收、抖动缓冲和恢复统计的观测工具
- **测试支撑**（`support/`）：提供断言工具、socket 辅助、H.264 样本、时钟工具

## 3. 子目录说明

| 子目录 | 作用 |
|---|---|
| `unit/` | 单元测试：纯逻辑验证，无网络、无图形 |
| `integration/` | 集成测试：本地 loopback 网络收发 |
| `smoke/` | 冒烟测试：最小可运行链路验证 |
| `benchmark/` | 基准测试：性能观测和回归工具 |
| `support/` | 测试支撑工具：断言、socket、H.264 样本、时钟 |

## 4. 主要文件说明

| 文件 | 作用 |
|---|---|
| `unit/CliOptionsTest.cpp` | CLI 参数解析测试：验证 `ParseSharedStreamOption`、`ParseClientOption`、`ParseUdpBenchmarkOption` |
| `unit/BoundedQueueTest.cpp` | 有界队列测试：验证 Push/TryPop/WaitPop/Close/多线程 |
| `unit/LatencyStatsTest.cpp` | 延迟统计测试：验证空统计、单采样、多采样、环形缓冲溢出、百分位计算 |
| `unit/RtpProtocolTest.cpp` | RTP 协议测试：验证 RTP 头解析、延迟扩展序列化/反序列化 |
| `unit/SdpSessionDescriptionTest.cpp` | SDP 解析测试：验证正常解析、文件加载、不完整 SDP 拒绝 |
| `integration/TcpReceiveIntegrationTest.cpp` | TCP 接收集成测试：本地 loopback 发送/接收，验证 metadata 和 payload |
| `integration/UdpReceiveIntegrationTest.cpp` | UDP 接收集成测试：验证 jitter buffer 跳帧、FEC 恢复、NACK 恢复 |
| `integration/RtpReceiveIntegrationTest.cpp` | RTP 接收集成测试：验证 Single NALU + FU-A 重组、损坏帧丢弃、延迟扩展恢复 |
| `smoke/RtpDecodeSmoke.cpp` | RTP 解码冒烟测试：进程内 RTP sender + 接收 + 解码，验证最小链路 |
| `benchmark/UdpJitterBenchmark.cpp` | UDP 抖动基准测试：接收指定帧数，输出延迟统计和恢复统计 |
| `support/TestAssertions.h` | 断言工具：`Expect(condition, message)` 和 `ExpectNear(actual, expected, tolerance, message)` |
| `support/SocketHelpers.h` | Socket 辅助：`SendAll()` 确保完整发送 |
| `support/H264Samples.h` | H.264 样本：内嵌的 SPS/PPS/IDR 帧数据，用于 smoke 测试 |
| `support/MonotonicClock.h` | 时钟工具：`MonotonicNowNs()` 获取单调时钟纳秒时间戳 |

## 5. 测试分层说明

### Unit（单元测试）

快速、稳定、无外部依赖。验证纯逻辑：

- `CliOptionsTest`：构造 argv → 调用解析函数 → 检查输出结构体字段
- `BoundedQueueTest`：单线程和多线程场景下的队列行为
- `LatencyStatsTest`：环形缓冲、百分位计算、格式化输出
- `RtpProtocolTest`：RTP 头序列化/反序列化、延迟扩展
- `SdpSessionDescriptionTest`：SDP 文本解析、文件加载、错误处理

### Integration（集成测试）

使用本地 loopback socket 进行真实网络收发：

- `TcpReceiveIntegrationTest`：启动 TCP 服务端 → 客户端连接 → 发送帧 → 接收验证
- `UdpReceiveIntegrationTest`：启动 UDP 服务端 → 客户端连接 → 测试 jitter buffer 跳帧、FEC 恢复、NACK 恢复
- `RtpReceiveIntegrationTest`：启动 RTP 发送端 → 客户端接收 → 测试 Single NALU/FU-A 重组、损坏帧处理、延迟扩展

### Smoke（冒烟测试）

验证最小可运行链路：

- `RtpDecodeSmoke`：内嵌 RTP sender 发送 H.264 SPS/PPS/IDR → 接收 → 解码 → 验证解码帧数
- 自动选择本地空闲端口，无需外部服务端
- 支持 `--self-test off` 切换到外部 RTP 流联调

### Benchmark（基准测试）

性能观测工具：

- `UdpJitterBenchmark`：接收指定帧数 → 统计延迟、抖动、丢包、NACK/FEC → 输出报告
- 注册到 ctest 的是 `--help` 检查，真正的 benchmark 按需手动运行

## 6. 与其他模块的关系

```
tests/
  ├── unit/ → 直接调用 common/ 和 app/cli/ 的函数
  ├── integration/ → 调用 modules/network/StreamClient，使用 support/ 工具
  ├── smoke/ → 调用 modules/network/StreamClient + modules/decoding/VideoDecoder
  ├── benchmark/ → 调用 modules/network/StreamClient + modules/decoding/VideoDecoder + common/metrics
  └── support/ → 被所有测试使用
```

## 7. 线程模型

- 单元测试：单线程（`BoundedQueueTest` 中有多线程测试用例）
- 集成测试：测试主线程 + 服务端/发送端线程
- 冒烟测试：主线程（接收+解码）+ sender 线程
- 基准测试：单线程

## 8. 运行方式

```bash
# 构建
cmake -S . -B build
cmake --build build -j

# 全量测试
ctest --test-dir build --output-on-failure

# 按标签筛选
ctest --test-dir build --output-on-failure -L unit
ctest --test-dir build --output-on-failure -L integration
ctest --test-dir build --output-on-failure -L smoke
ctest --test-dir build --output-on-failure -L benchmark

# 单独运行 smoke
./build/rtp_decode_smoke

# 单独运行 benchmark
./build/udp_jitter_benchmark --help
./build/udp_jitter_benchmark --frames 120 --decode on
```

## 9. 调试建议

- **单元测试失败**：检查对应模块的实现，断言消息会输出到 stderr
- **集成测试超时**：检查 loopback socket 是否正常，可能是端口冲突
- **Smoke 测试失败**：检查 FFmpeg 是否可用，H.264 解码器是否正常
- **Benchmark 输出异常**：检查 UDP 统计字段，对比预期值

## 10. 后续扩展方向

- 引入测试框架（Google Test、Catch2）
- 增加更多单元测试覆盖
- 增加性能回归检测
- 增加代码覆盖率统计
- 增加 fuzzing 测试
