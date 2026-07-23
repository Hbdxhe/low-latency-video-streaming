# src/common/metrics

## 1. 模块定位

度量工具子模块。提供延迟统计能力，用于测量管线各阶段的耗时，是性能观测和 HUD 展示的数据源。

## 2. 核心职责

- 提供环形缓冲区存储延迟采样
- 计算 min/avg/p50/p95/p99/max 百分位统计
- 支持格式化输出（`Format()`）
- 支持增量快照（避免每次都排序）

## 3. 主要文件说明

| 文件 | 作用 |
|---|---|
| `LatencyStats.h` | 延迟统计类，header-only 实现 |

## 4. 核心类 / 函数说明

### LatencyStats

作用：
- 环形缓冲区实现的延迟统计
- 被 `main.cpp` 用于统计管线各阶段延迟
- 被 `AdaptiveJitterBuffer` 用于统计网络抖动

关键函数：
- `Record(double value_ms)`：记录一个延迟采样
- `Snapshot()`：返回 `LatencySummary`（触发排序计算百分位）
- `Format(name)`：格式化输出统计摘要，如 `receive_to_render count=120 min=5.23ms avg=8.45ms p50=7.80ms p95=12.30ms p99=15.60ms max=20.10ms last=8.90ms`
- `has_samples()`：是否有采样数据
- `last_ms()`：最近一次采样值
- `Reset()`：清空所有采样

### LatencySummary

- 纯数据结构，包含 count/last/min/avg/p50/p95/p99/max
- 用于传递给 HUD 渲染

## 5. 数据流说明

输入：
- `main.cpp` 中各阶段时间戳差值通过 `Record()` 写入

输出：
- `Snapshot()` 返回给 `RenderFrameInfo`，供 HUD 展示
- `Format()` 输出到日志

## 6. 与其他模块的关系

- 被 `app/main.cpp` 使用：统计管线延迟
- 被 `modules/network/AdaptiveJitterBuffer` 使用：统计网络抖动
- `LatencySummary` 被 `modules/rendering/VideoRenderer.h` 的 `RenderFrameInfo` 使用

## 7. 线程模型 / 队列模型

本模块当前不直接管理线程。`LatencyStats` 非线程安全，在 `main.cpp` 中仅在渲染线程访问。`AdaptiveJitterBuffer` 中的使用也是单线程。

## 8. 配置参数

- 构造参数 `max_samples`：环形缓冲区大小，默认 4096
- 构造参数 `snapshot_refresh_interval`：快照刷新间隔（采样次数），默认 8

## 9. 调试建议

- 观察 `has_samples()` 确认是否有数据
- 调用 `Format()` 输出到日志查看各阶段延迟
- 如果 p95 延迟异常高，检查对应阶段的实现

## 10. 后续扩展方向

- 增加线程安全版本
- 增加直方图统计
- 增加滑动窗口统计（而非环形缓冲）
- 增加导出到 Prometheus/Grafana 的能力
