#include "send_queue.hpp"
#include <algorithm>
#include <chrono>

namespace cclient {
SendQueue::SendQueue(cnet::Socket& sock, std::size_t max_frames, std::size_t max_bytes,
                     std::size_t chunk_bytes, std::size_t chunk_delay_ms,
                     TimeFn clock, CompletionFn completed)
    : sock_(sock), max_frames_(max_frames), max_bytes_(max_bytes),
      chunk_bytes_(chunk_bytes), chunk_delay_ms_(chunk_delay_ms),
      clock_(std::move(clock)), completed_(std::move(completed)) {
    worker_ = std::thread([this] { run(); });
}

SendQueue::~SendQueue() {
    close();
}

void SendQueue::close() {
    stop();
    if (worker_.joinable()) worker_.join();
}

bool SendQueue::push(mavlink_gateway::Bytes data, bool priority, TxTag tag) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!accepting_ || stop_ || failed_) return false;
    if (queue_.size() + priority_queue_.size() >= max_frames_ || data.size() > max_bytes_ - queued_bytes_) {
        ++dropped_;
        ++queue_full_;
        return false;
    }
    queued_bytes_ += data.size();
    (priority ? priority_queue_ : queue_).push_back({std::move(data), tag});
    peak_queued_frames_ = std::max(peak_queued_frames_, queue_.size() + priority_queue_.size());
    wake_.notify_one();
    return true;
}

void SendQueue::stop() {
    std::deque<Item> cancelled, priority_cancelled;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        stop_ = true;
        accepting_ = false;
        dropped_ += queue_.size() + priority_queue_.size();
        cancelled.swap(queue_);
        priority_cancelled.swap(priority_queue_);
        queued_bytes_ = 0;
    }
    wake_.notify_all();
    sock_.shutdown_both();
    for (const auto& item : priority_cancelled) notify(item, mavlink_gateway::DeliveryStatus::ShutdownDiscard);
    for (const auto& item : cancelled) notify(item, mavlink_gateway::DeliveryStatus::ShutdownDiscard);
    drained_.notify_all();
}

bool SendQueue::drain_until(std::chrono::steady_clock::time_point deadline) {
    std::unique_lock<std::mutex> lock(mutex_);
    accepting_ = false;
    return drained_.wait_until(lock, deadline, [this] {
        return queue_.empty() && priority_queue_.empty() && !in_flight_;
    });
}

SendQueueSnapshot SendQueue::snapshot() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return {socket_bytes_.load(), sent_.load(), queue_full_.load(), dropped_.load(),
            queue_.size() + priority_queue_.size(), queued_bytes_,
            queue_.size() + priority_queue_.size() + (in_flight_ ? 1U : 0U), peak_queued_frames_,
            failed_.load(), observer_failed_.load()};
}

void SendQueue::notify(const Item& item, mavlink_gateway::DeliveryStatus status) noexcept {
    if (!completed_) return;
    try { completed_({item.tag, clock_ ? clock_() : 0, item.bytes.size(), status}); }
    catch (...) { observer_failed_ = true; }
}

bool SendQueue::send_frame(const mavlink_gateway::Bytes& data) {
    for (std::size_t offset = 0; offset < data.size();) {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            if (stop_) return false;
        }
        const auto amount = chunk_bytes_ ? std::min(chunk_bytes_, data.size() - offset) : data.size() - offset;
        if (!sock_.send_all(data.data() + offset, amount, &socket_bytes_)) return false;
        offset += amount;
        if (offset < data.size() && chunk_delay_ms_) {
            std::unique_lock<std::mutex> lock(mutex_);
            if (wake_.wait_for(lock, std::chrono::milliseconds(chunk_delay_ms_), [this] { return stop_; }))
                return false;
        }
    }
    return true;
}

void SendQueue::run() {
    for (;;) {
        Item item;
        {
            std::unique_lock<std::mutex> lock(mutex_);
            wake_.wait(lock, [this] { return stop_ || !queue_.empty() || !priority_queue_.empty(); });
            if (stop_) break;
            auto& pending = priority_queue_.empty() ? queue_ : priority_queue_;
            item = std::move(pending.front());
            pending.pop_front();
            queued_bytes_ -= item.bytes.size();
            in_flight_ = true;
        }
        // Gui KHONG giu khoa.
        if (!send_frame(item.bytes)) {
            ++dropped_;
            mavlink_gateway::DeliveryStatus status;
            {
                std::lock_guard<std::mutex> lock(mutex_);
                if (!stop_) failed_ = true;
                status = stop_ ? mavlink_gateway::DeliveryStatus::ShutdownDiscard : mavlink_gateway::DeliveryStatus::SendError;
            }
            notify(item, status);
            { std::lock_guard<std::mutex> lock(mutex_); in_flight_ = false; }
            drained_.notify_all();
            stop();
            return;
        }
        sent_ += item.bytes.size();
        notify(item, mavlink_gateway::DeliveryStatus::SentToSocket);
        { std::lock_guard<std::mutex> lock(mutex_); in_flight_ = false; }
        drained_.notify_all();
    }
}
} // namespace cclient
