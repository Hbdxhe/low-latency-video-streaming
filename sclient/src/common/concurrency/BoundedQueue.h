#ifndef SCLIENT_BOUNDEDQUEUE_H
#define SCLIENT_BOUNDEDQUEUE_H

#include <algorithm>
#include <condition_variable>
#include <cstddef>
#include <deque>
#include <mutex>
#include <utility>

namespace sclient {

/**
 * 线程安全的有界队列
 *
 * 支持多生产者-多消费者模式。当队列满时，新元素会替换最旧的元素。
 * 提供阻塞和非阻塞两种出队方式，支持优雅关闭。
 */
template <typename T>
class BoundedQueue {
public:
    /**
     * 构造函数
     * @param capacity 队列最大容量，最小为1
     */
    explicit BoundedQueue(std::size_t capacity)
            : capacity_(std::max<std::size_t>(1, capacity)),
              closed_(false) {
    }

    /**
     * 推入元素，队列满时丢弃最旧的元素
     *
     * 这种策略确保生产者不会被阻塞，适用于实时性要求高的场景。
     *
     * @param value 要推入的元素
     * @return 成功推入返回true，队列已关闭返回false
     */
    bool PushOrDropOldest(T value) {
        std::lock_guard<std::mutex> lock(mutex_);
        if (closed_) {
            return false;
        }

        // 队列已满时，移除最旧的元素为新元素腾出空间
        if (queue_.size() >= capacity_) {
            queue_.pop_front();
        }

        queue_.push_back(std::move(value));
        condition_.notify_one();
        return true;
    }

    /**
     * 阻塞等待并弹出队首元素
     *
     * 会阻塞当前线程直到有元素可弹出或队列被关闭。
     * 适用于消费者需要确保处理每个元素的场景。
     *
     * @param value 用于存储弹出元素的指针
     * @return 成功弹出返回true，队列为空或已关闭返回false
     */
    bool WaitPop(T *value) {
        if (value == nullptr) {
            return false;
        }

        std::unique_lock<std::mutex> lock(mutex_);
        // 等待直到队列非空或已关闭
        condition_.wait(lock, [this]() {
            return closed_ || !queue_.empty();
        });
        // 队列已关闭且为空时返回失败
        if (queue_.empty()) {
            return false;
        }

        *value = std::move(queue_.front());
        queue_.pop_front();
        return true;
    }

    /**
     * 非阻塞尝试弹出队首元素
     *
     * 立即返回，不阻塞等待。适用于轮询或可选处理的场景。
     *
     * @param value 用于存储弹出元素的指针
     * @return 成功弹出返回true，队列为空返回false
     */
    bool TryPop(T *value) {
        if (value == nullptr) {
            return false;
        }

        std::lock_guard<std::mutex> lock(mutex_);
        if (queue_.empty()) {
            return false;
        }

        *value = std::move(queue_.front());
        queue_.pop_front();
        return true;
    }

    /**
     * 获取当前队列元素数量
     *
     * 注意：在多线程环境下，返回值可能已过时
     */
    std::size_t Size() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return queue_.size();
    }

    /**
     * 关闭队列
     *
     * 关闭后，所有等待中的消费者会被唤醒，Push操作将失败。
     * 这是优雅停止消费者线程的标准方式。
     */
    void Close() {
        std::lock_guard<std::mutex> lock(mutex_);
        closed_ = true;
        condition_.notify_all();
    }

private:
    std::size_t capacity_;
    bool closed_;
    std::deque<T> queue_;
    mutable std::mutex mutex_;
    std::condition_variable condition_;
};

}  // namespace sclient

#endif  // SCLIENT_BOUNDEDQUEUE_H
