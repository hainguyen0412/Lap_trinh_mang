#pragma once
#include <chrono>
#include <condition_variable>
#include <deque>
#include <mutex>
#include <utility>

namespace mavlink_gateway {
// Weight is raw bytes for a client queue, 1 for datagram/uplink queues.
template<class T> class BoundedQueue {
    struct Entry { T value; std::size_t weight; };
    std::deque<Entry> items_;
    mutable std::mutex mutex_;
    std::condition_variable cv_;
    std::size_t max_count_, max_weight_, weight_ = 0;
    bool closed_ = false;
public:
    BoundedQueue(std::size_t count, std::size_t weight) : max_count_(count), max_weight_(weight) {}
    bool try_push(T item, std::size_t weight = 1) {
        std::lock_guard<std::mutex> lock(mutex_);
        if (closed_ || items_.size() >= max_count_ || weight > max_weight_ - weight_) return false;
        items_.push_back({std::move(item), weight}); weight_ += weight;
        cv_.notify_one(); return true;
    }
    bool pop(T& out, int timeout_ms = 50) {
        std::unique_lock<std::mutex> lock(mutex_);
        cv_.wait_for(lock, std::chrono::milliseconds(timeout_ms), [&] { return closed_ || !items_.empty(); });
        if (items_.empty()) return false;
        auto entry = std::move(items_.front()); items_.pop_front(); weight_ -= entry.weight;
        out = std::move(entry.value); return true;
    }
    void close() { std::lock_guard<std::mutex> lock(mutex_); closed_ = true; cv_.notify_all(); }
    bool closed() const { std::lock_guard<std::mutex> lock(mutex_); return closed_; }
    std::size_t size() const { std::lock_guard<std::mutex> lock(mutex_); return items_.size(); }
};
} // namespace mavlink_gateway
