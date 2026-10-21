/*
 * RingBuffer.h — 线程安全环形队列（模板）
 *
 * 设计要点（视频流式场景）：
 *   1. 生产者 push：队列满时【丢弃最旧帧】再写入，不等待空位，
 *      保证采集线程不被拖慢，避免内存持续上涨导致 OOM；
 *   2. 消费者 pop：阻塞等待，直到有数据或队列被 stop；
 *   3. 内部用 std::mutex + std::condition_variable 实现，无虚假唤醒问题
 *      （wait 使用带谓词的重载，条件不满足自动继续等待）；
 *   4. stop() 后所有阻塞中的消费者立即返回 false，用于优雅退出。
 *
 * 线程安全：push/pop/size/stop 全部在互斥锁保护下操作。
 */
#ifndef MULTI_STREAM_RINGBUFFER_H
#define MULTI_STREAM_RINGBUFFER_H

#include <condition_variable>
#include <cstddef>
#include <deque>
#include <mutex>
#include <chrono>

namespace msad {

template <typename T>
class RingBuffer {
public:
    explicit RingBuffer(size_t capacity) : capacity_(capacity) {
        if (capacity_ == 0) capacity_ = 1;   /* 至少容纳 1 条 */
    }

    /*
     * 生产者入队：满则丢弃最旧元素。
     * 不等待空位；互斥锁可能短暂等待，内存分配仍可能抛异常。
     */
    void push(T item) {
        {
            std::lock_guard<std::mutex> lock(mtx_);
            if (stopped_) return;             /* 已停止则丢弃 */
            if (buf_.size() >= capacity_) {
                buf_.pop_front();             /* 丢弃最旧帧 */
                ++droppedCount_;              /* 统计丢弃帧数（用于观测） */
            }
            buf_.push_back(std::move(item));
        }
        cv_.notify_one();                     /* 唤醒一个消费者 */
    }

    /*
     * 消费者出队：阻塞等待直到有数据或队列停止。
     * @return true=取到数据；false=队列已停止且为空（用于线程退出判断）
     */
    bool pop(T& out) {
        std::unique_lock<std::mutex> lock(mtx_);
        cv_.wait(lock, [this] { return stopped_ || !buf_.empty(); });
        if (buf_.empty()) return false;       /* 停止后无数据 */
        out = std::move(buf_.front());
        buf_.pop_front();
        return true;
    }

    enum class PushResult { Accepted, Full, Stopped };
    // Event queues preserve accepted work; full queues reject the new item.
    PushResult tryPush(T item) {
        {
            std::lock_guard<std::mutex> lock(mtx_);
            if (stopped_) return PushResult::Stopped;
            if (buf_.size() >= capacity_) { ++rejectedCount_; return PushResult::Full; }
            buf_.push_back(std::move(item));
        }
        cv_.notify_one();
        return PushResult::Accepted;
    }
    size_t rejectedCount() const { std::lock_guard<std::mutex> lock(mtx_); return rejectedCount_; }
    size_t cancelledCount() const { std::lock_guard<std::mutex> lock(mtx_); return cancelledCount_; }

    /* 当前队列长度（调试用） */
    size_t size() const {
        std::lock_guard<std::mutex> lock(mtx_);
        return buf_.size();
    }

    /* 累计丢弃的元素数（观测队列压力） */
    size_t droppedCount() const {
        std::lock_guard<std::mutex> lock(mtx_);
        return droppedCount_;
    }

    /* 停止队列：唤醒所有阻塞消费者，之后 push 直接丢弃 */
    void stop(bool drain = false) {
        {
            std::lock_guard<std::mutex> lock(mtx_);
            stopped_ = true;
            if (!drain) { cancelledCount_ += buf_.size(); buf_.clear(); } //                     /* 清空残留数据 */
        }
        cv_.notify_all();                     /* 唤醒全部消费者 */
    }

private:
    mutable std::mutex mtx_;                  /* 保护 buf_ */
    std::condition_variable cv_;
    std::deque<T> buf_;                       /* 底层用 deque 便于头尾操作 */
    size_t capacity_ = 0;
    size_t droppedCount_ = 0;
    size_t rejectedCount_ = 0;
    size_t cancelledCount_ = 0;
    bool stopped_ = false;
};

} // namespace msad

#endif // MULTI_STREAM_RINGBUFFER_H
