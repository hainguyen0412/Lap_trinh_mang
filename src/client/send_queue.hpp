#pragma once
// Hang doi gui + DUNG MOT worker thread cho moi socket TCP.
// Moi noi muon gui (heartbeat, lenh) chi push() vao day; khong ai goi send() truc tiep,
// nen byte cua hai khung khong bao gio xen ke nhau.
#include "mavlink_gateway/contracts.hpp"
#include "net_socket.hpp"
#include "tx_record.hpp"
#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <mutex>
#include <thread>
#include <functional>
#include <chrono>

namespace cclient {
struct SendQueueSnapshot {
    std::uint64_t socket_bytes = 0, complete_bytes = 0, queue_full = 0, dropped = 0;
    std::size_t queued_frames = 0, queued_bytes = 0, pending_frames = 0, peak_queued_frames = 0;
    bool failed = false, observer_failed = false;
};
class SendQueue {
public:
    using TimeFn = std::function<mavlink_gateway::MonoNs()>;
    using CompletionFn = std::function<void(const SendCompletion&)>;
    explicit SendQueue(cnet::Socket& sock, std::size_t max_frames = 1024,
                       std::size_t max_bytes = 1048576,
                       std::size_t chunk_bytes = 0, std::size_t chunk_delay_ms = 0,
                       TimeFn clock = {}, CompletionFn completed = {});
    ~SendQueue();
    SendQueue(const SendQueue&) = delete;
    SendQueue& operator=(const SendQueue&) = delete;

    // false neu hang doi day, da dung, hoac socket loi. Khong chan.
    // Priority frames retain FIFO order and start only after the current frame finishes.
    bool push(mavlink_gateway::Bytes data, bool priority = false, TxTag tag = {});
    bool drain_until(std::chrono::steady_clock::time_point deadline);
    SendQueueSnapshot snapshot() const;
    // Discard pending frames and shutdown socket I/O to wake blocked send/recv before join.
    void stop();
    void close(); // Stop and join the send worker before inspecting final counters.
    bool failed() const { return failed_; }
    // Bytes of complete frames accepted by send_all; this does not confirm peer receipt.
    std::uint64_t bytes_sent() const { return sent_; }
    // Queue-full rejections plus queued/in-flight frames cancelled before completion.
    std::uint64_t dropped() const { return dropped_; }

private:
    void run();
    bool send_frame(const mavlink_gateway::Bytes& data);
    struct Item { mavlink_gateway::Bytes bytes; TxTag tag; };
    void notify(const Item& item, mavlink_gateway::DeliveryStatus status) noexcept;
    cnet::Socket& sock_;
    const std::size_t max_frames_, max_bytes_, chunk_bytes_, chunk_delay_ms_;
    const TimeFn clock_;
    const CompletionFn completed_;
    mutable std::mutex mutex_;
    std::condition_variable wake_, drained_;
    std::deque<Item> queue_, priority_queue_;
    std::size_t queued_bytes_ = 0, peak_queued_frames_ = 0;
    std::atomic<bool> stop_{false};
    bool accepting_ = true, in_flight_ = false;
    std::atomic<bool> failed_{false};
    std::atomic<std::uint64_t> sent_{0}, dropped_{0};
    std::atomic<std::uint64_t> socket_bytes_{0}, queue_full_{0};
    std::atomic<bool> observer_failed_{false};
    std::thread worker_;
};
} // namespace cclient
