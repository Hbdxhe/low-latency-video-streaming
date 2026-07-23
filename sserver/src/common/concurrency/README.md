# common/concurrency

## 1. 模块定位

并发工具。提供线程安全的队列模板，支持多种入队策略和阻塞等待。

## 2. 核心职责

- 提供 `ThreadSafeQueue<T>` 模板类
- 支持阻塞等待出队（`WaitPopFor`）
- 支持丢弃最旧帧入队（`PushDropOldest`）
- 支持选择性丢弃入队（`PushDropSelective`）
- 支持通知所有等待者（`NotifyAll`）

## 3. 主要文件说明

| 文件 | 作用 |
|---|---|
| ThreadSafeQueue.h | 定义 `ThreadSafeQueue<T>` 模板类，header-only 实现 |

## 4. 核心类 / 函数说明

### ThreadSafeQueue<T>

作用：
- 基于 `std::deque<T>`、`std::mutex`、`std::condition_variable` 实现的线程安全队列
- 被 `CaptureModule` 用于采集帧的跨线程传递
- 被 `TcpClientSession` 用于发送帧的跨线程传递

关键函数：
- `Push(value)`：无界入队，通知一个等待者
- `PushDropOldest(value, max_size)`：有界入队，队满时丢弃最旧元素
- `PushDropOldestCountDropped(value, max_size)`：同上，但返回丢弃数量
- `PushDropSelective(value, max_size, predicate)`：有界入队，队满时优先丢弃满足 predicate 的元素，不满足则丢弃最旧
- `WaitPopFor(value, timeout)`：阻塞等待出队，超时返回 false
- `TryPop(value)`：非阻塞尝试出队
- `NotifyAll()`：唤醒所有等待中的 `WaitPopFor`，用于停止时通知
- `Clear()`：清空队列
- `Size()` / `Empty()`：查询队列状态
- `AnyMatching(predicate)`：查询队列中是否存在满足条件的元素

## 5. 线程模型

所有公开方法都是线程安全的。内部使用 `std::mutex` 保护队列操作，使用 `std::condition_variable` 实现阻塞等待。

## 6. 与其他模块的关系

- 被 `CaptureModule` 使用：`raw_queue_` 传递原始帧从采集线程到编码线程
- 被 `TcpClientSession` 使用：`outbound_frames_` 传递编码帧从接收线程到发送线程
