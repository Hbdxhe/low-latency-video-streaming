# tests/support

## 1. 模块定位

测试支撑工具目录。提供被所有测试共享的基础设施：断言、socket 辅助、H.264 样本和时钟工具。

## 2. 核心职责

- 提供轻量断言工具（`Expect`、`ExpectNear`）
- 提供 socket 完整发送辅助（`SendAll`）
- 提供内嵌的 H.264 IDR 帧样本
- 提供单调时钟工具（`MonotonicNowNs`）

## 3. 主要文件说明

| 文件 | 作用 |
|---|---|
| `TestAssertions.h` | 断言工具：`Expect(condition, message)` 和 `ExpectNear(actual, expected, tolerance, message)` |
| `SocketHelpers.h` | Socket 辅助：`SendAll(fd, data, size)` 确保完整发送 |
| `H264Samples.h` | H.264 样本：内嵌的 SPS/PPS/IDR 帧（64x48 红色帧，由 ffmpeg 生成） |
| `MonotonicClock.h` | 时钟工具：`MonotonicNowNs()` 获取 `CLOCK_MONOTONIC` 纳秒时间戳 |

## 4. 核心函数说明

### Expect(condition, message)

- 条件为 true 返回 true
- 条件为 false 输出 `test failure: {message}` 到 stderr 并返回 false
- 被所有测试使用

### ExpectNear(actual, expected, tolerance, message)

- `|actual - expected| <= tolerance` 返回 true
- 否则输出详细信息到 stderr

### SendAll(fd, data, size)

- 循环 `send()` 直到所有数据发送完毕
- 处理部分发送的情况
- 被 TCP 集成测试使用

### EmbeddedH264IdrFrameNalus()

- 返回 `vector<vector<uint8_t>>`，包含 3 个 NALU：SPS、PPS、IDR
- 被 smoke 测试使用

### MonotonicNowNs()

- 返回 `CLOCK_MONOTONIC` 的纳秒时间戳
- 被集成测试和 benchmark 使用

## 5. 与其他模块的关系

- 被 `tests/unit/`、`tests/integration/`、`tests/smoke/`、`tests/benchmark/` 使用
- 不依赖 src/ 下的业务模块

## 6. 运行方式

本目录不包含独立的测试可执行文件，只提供头文件工具。
