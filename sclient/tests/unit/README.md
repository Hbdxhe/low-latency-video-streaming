# tests/unit

## 1. 模块定位

单元测试目录。包含纯逻辑验证的测试，无网络、无图形依赖，运行速度快。

## 2. 核心职责

- 验证 CLI 参数解析逻辑
- 验证有界队列的并发行为
- 验证延迟统计的数学正确性
- 验证 RTP 协议序列化/反序列化
- 验证 SDP 文件解析

## 3. 主要文件说明

| 文件 | 被测模块 | 测试内容 |
|---|---|---|
| `CliOptionsTest.cpp` | `app/cli/` | `ParseSharedStreamOption`、`ParseClientOption`、`ParseUdpBenchmarkOption` 的各种参数组合 |
| `BoundedQueueTest.cpp` | `common/concurrency/` | Push/TryPop/WaitPop、容量溢出丢弃、Close 唤醒、多线程生产者消费者 |
| `LatencyStatsTest.cpp` | `common/metrics/` | 空统计、单采样、多采样、环形缓冲溢出、百分位计算、格式化输出 |
| `RtpProtocolTest.cpp` | `common/net/` | RTP 头写入/解析、延迟扩展序列化/反序列化、无效扩展拒绝 |
| `SdpSessionDescriptionTest.cpp` | `common/net/` | SDP 文本解析、临时文件加载、不完整 SDP 拒绝 |

## 4. 核心测试函数说明

### CliOptionsTest

- `TestParseSharedStreamOptions()`：验证 `--host`、`--sdp`、`--udp-jitter-buffer`、`--decoder` 解析
- `TestClientOptionParsesRendererFlags()`：验证 `--transport`、`--renderer`、`--vsync`、`--window-title` 解析
- `TestUdpBenchmarkSpecificOptions()`：验证 `--frames`、`--decode`、`--inject-jitter-pattern`、`--inject-jitter-period` 解析
- `TestInvalidAndHelpPaths()`：验证无效参数报错和 `--help` 识别

### BoundedQueueTest

- `TestPushAndTryPop()`：基本入队/出队
- `TestPushOrDropOldestAtCapacity()`：队列满时丢弃最旧元素
- `TestTryPopWithNullPointer()` / `TestWaitPopWithNullPointer()`：空指针防护
- `TestCloseWakesWaitPop()`：Close 唤醒阻塞的 WaitPop
- `TestCloseReturnsPendingData()`：Close 后仍可取出已入队的数据
- `TestPushFailsAfterClose()`：Close 后入队返回 false
- `TestWaitPopBlocksUntilDataAvailable()`：WaitPop 阻塞直到有数据
- `TestMultiThreadedProducerConsumer()`：多线程生产者消费者场景

### LatencyStatsTest

- `TestEmptyStats()`：空统计的状态
- `TestSingleSample()`：单采样的统计值
- `TestMultipleSamples()`：10 个采样的 min/avg/p50
- `TestCircularBufferOverflow()`：环形缓冲溢出后只保留最新数据
- `TestPercentileCalculation()`：100 个采样的 p50/p95/p99
- `TestFormatOutput()`：格式化输出包含预期字段

### RtpProtocolTest

- `TestParsesLatencyExtension()`：写入带延迟扩展的 RTP 包 → 解析 → 验证字段一致
- `TestRejectsInvalidLatencyExtension()`：无效 profile ID 的扩展被正确拒绝

### SdpSessionDescriptionTest

- `TestParsesSserverRtpSdp()`：解析完整 SDP → 验证 address/port/payload_type/clock_rate
- `TestLoadsSdpFromFile()`：写临时文件 → 加载 → 验证
- `TestRejectsIncompleteSdp()`：缺少必要字段的 SDP 被拒绝

## 5. 与其他模块的关系

- 直接调用 `app/cli/`、`common/concurrency/`、`common/metrics/`、`common/net/` 的函数
- 使用 `tests/support/TestAssertions.h` 的断言工具

## 6. 运行方式

```bash
ctest --test-dir build --output-on-failure -L unit
```

超时限制：每个测试 5 秒。
