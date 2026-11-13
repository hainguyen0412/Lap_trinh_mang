#pragma once
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <functional>
#include <mutex>
#include <thread>

namespace cclient {
class PeriodicSampler {
public:
    PeriodicSampler(std::chrono::milliseconds interval, std::function<void()> sample)
        : worker_([this, interval, sample = std::move(sample)] {
            std::unique_lock<std::mutex> lock(mutex_);
            while (!stop_) {
                lock.unlock();
                try { sample(); }
                catch (const std::exception& error) {
                    failed_ = true;
                    std::fprintf(stderr, "Metrics sampler: %s\n", error.what());
                    return;
                }
                lock.lock();
                wake_.wait_for(lock, interval, [this] { return stop_; });
            }
        }) {}
    ~PeriodicSampler() { stop(); }
    void stop() {
        { std::lock_guard<std::mutex> lock(mutex_); stop_ = true; }
        wake_.notify_all();
        if (worker_.joinable()) worker_.join();
    }
    bool failed() const { return failed_; }
private:
    std::mutex mutex_;
    std::condition_variable wake_;
    bool stop_ = false;
    std::atomic<bool> failed_{false};
    std::thread worker_;
};
} // namespace cclient
