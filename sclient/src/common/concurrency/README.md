# src/common/concurrency

## 1. 模块定位

并发工具子模块。当前提供一个通用的有界线程安全队列，用于在生产者-消费者模式下在线程之间安全传递数据。

## 2. 核心职责

- 提供线程安全的有界队列 `BoundedQueue<T>`
- 支持阻塞等待弹出（`WaitPop`）
- 支持非阻塞尝试弹出（`TryPop`）
- 支持满时丢弃最旧元素（`PushOrDropOldest`）
- 支持关闭队列并唤醒所有等待者（`Close`）

## 3. 主要文件说明

| 文件 | 作用 |
|---|---|
| `BoundedQueue.h` | 模板有界队列，header-only 实现 |

## 4. 核心类 / 函数说明

### BoundedQueue\<T\>

作用：
- 线程安全的有界队列
- 在 `main.cpp` 中用于接收线程→解码线程、解码线程→渲染线程的数据传递
- 采用"丢弃最旧"策略而非阻塞生产者，适合实时视频场景

关键函数：
- `PushOrDropOldest(T value)`：入队。队列满时丢弃最旧元素，返回 false 表示队列已关闭
- `WaitPop(T *value)`：阻塞等待直到有数据或队列关闭
- `TryPop(T *value)`：非阻塞尝试弹出，无数据返回 false
- `Size()`：当前队列大小
- `Close()`：关闭队列，唤醒所有 `WaitPop` 等待者

## 5. 数据流说明

输入：
- 生产者线程调用 `PushOrDropOldest()` 推入数据

输出：
- 消费者线程调用 `WaitPop()` 或 `TryPop()` 取出数据

## 6. 与其他模块的关系

- 被 `app/main.cpp` 使用：`BoundedQueue<ReceivedFrame>` 和 `BoundedQueue<PipelineDecodedFrame>`
- 不依赖其他模块

## 7. 线程模型 / 队列模型

本模块本身就是线程同步原语：
- 内部使用 `std::mutex` 保护队列状态
- 使用 `std::condition_variable` 实现 `WaitPop` 的阻塞等待
- `Close()` 调用 `notify_all()` 唤醒所有等待者
- `PushOrDropOldest` 调用 `notify_one()` 通知一个等待的消费者

## 8. 配置参数

- 构造参数：`capacity`（队列容量，最小为 1）

## 9. 调试建议

- 观察队列深度：调用 `Size()` 检查是否存在积压
- 观察丢帧：`PushOrDropOldest` 返回 true 但队列已满时实际发生了丢帧
- 适合打断点：`PushOrDropOldest` 中 `queue_.pop_front()` 处，观察丢帧频率

## 10. 后续扩展方向

- 增加多种丢帧策略（丢最新、丢最旧、阻塞生产者）
- 增加队列深度变化的回调通知
- 增加无锁队列实现（适合高吞吐场景）
