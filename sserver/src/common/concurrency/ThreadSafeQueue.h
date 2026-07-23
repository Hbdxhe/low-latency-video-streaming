#ifndef SSERVER_COMMON_CONCURRENCY_THREADSAFEQUEUE_H
#define SSERVER_COMMON_CONCURRENCY_THREADSAFEQUEUE_H

#include <cstddef>
#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <deque>
#include <mutex>

namespace sserver {
namespace common {
namespace concurrency {

// 线程安全的双端队列，支持阻塞等待弹出、丢弃最旧元素、按条件选择性丢弃等策略
template <typename T>
// 通用线程安全双端队列。
//
// 内部用 mutex 保护 deque，用 condition_variable 唤醒等待消费者。
// 支持普通 FIFO、队满丢最旧元素、按条件优先丢弃等低延迟策略。
class ThreadSafeQueue {
public:
    // 普通入队，不限制长度。
    void Push(const T &value) {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            queue_.push_back(value);
        }
        // 离开锁作用域后再唤醒一个消费者。
        condition_variable_.notify_one();
    }

    // 有界入队：达到 max_size 时丢弃最旧元素。
    void PushDropOldest(const T &value, std::size_t max_size) {
        PushDropOldestCountDropped(value, max_size);
    }

    // 和 PushDropOldest 相同，但返回本次丢弃数量。
    std::size_t PushDropOldestCountDropped(const T &value, std::size_t max_size) {
        std::size_t dropped = 0;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            while (queue_.size() >= max_size && !queue_.empty()) {
                queue_.pop_front();
                ++dropped;
            }
            queue_.push_back(value);
        }
        condition_variable_.notify_one();
        return dropped;
    }

    template <typename Predicate>
    std::size_t PushDropSelective(const T &value, std::size_t max_size, Predicate predicate) {
        // 队满时优先删除满足 predicate 的元素，找不到时退化为删除最旧元素。
        // 传输队列可用它优先丢非关键帧。
        std::size_t dropped = 0;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            while (queue_.size() >= max_size && !queue_.empty()) {
                typename std::deque<T>::iterator candidate = std::find_if(queue_.begin(), queue_.end(), predicate);
                if (candidate != queue_.end()) {
                    queue_.erase(candidate);
                } else {
                    queue_.pop_front();
                }
                ++dropped;
            }
            queue_.push_back(value);
        }
        condition_variable_.notify_one();
        return dropped;
    }

    bool WaitPopFor(T *value, const std::chrono::milliseconds &timeout) {
        // 等待期间自动释放 mutex；有数据或超时后重新加锁。
        std::unique_lock<std::mutex> lock(mutex_);
        if (!condition_variable_.wait_for(lock, timeout, [this]() { return !queue_.empty(); })) {
            return false;
        }
        *value = queue_.front();
        queue_.pop_front();
        return true;
    }

    void NotifyAll() {
        // 停止模块时唤醒所有等待线程，避免 join 长时间阻塞。
        condition_variable_.notify_all();
    }

    void Clear() {
        // 清空队列。
        std::lock_guard<std::mutex> lock(mutex_);
        queue_.clear();
    }

    bool TryPop(T *value) {
        // 非阻塞弹出；队列为空时立即返回 false。
        std::lock_guard<std::mutex> lock(mutex_);
        if (queue_.empty()) {
            return false;
        }
        *value = queue_.front();
        queue_.pop_front();
        return true;
    }

    bool Empty() const {
        // const 查询也需要加锁，因此 mutex_ 使用 mutable。
        std::lock_guard<std::mutex> lock(mutex_);
        return queue_.empty();
    }

    std::size_t Size() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return queue_.size();
    }

    // 用于判断队列中是否存在符合条件的数据
    template <typename Predicate>
    bool AnyMatching(Predicate predicate) const {
        std::lock_guard<std::mutex> lock(mutex_);
        return std::find_if(queue_.begin(), queue_.end(), predicate) != queue_.end();
    }

private:
    // 保护 queue_。
    mutable std::mutex mutex_;
    // 用于消费者等待新元素。
    std::condition_variable condition_variable_;
    // 头部出队、尾部入队。
    std::deque<T> queue_;
};

}  // namespace concurrency
}  // namespace common
}  // namespace sserver

#endif  // SSERVER_COMMON_CONCURRENCY_THREADSAFEQUEUE_H
