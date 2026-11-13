#pragma once
#include "tx_record.hpp"
#include <functional>
#include <mutex>
#include <optional>
#include <vector>

namespace cclient {
using mavlink_gateway::MonoNs;
struct MeasurementSnapshot {
    bool connected = false, stopped = false;
    MonoNs connected_at_ns = 0, stopped_at_ns = 0, window_start_ns = 0,
           window_end_ns = 0, delivery_cutoff_ns = 0;
    double measurement_s = 0;
    std::uint64_t valid_total = 0, valid_window = 0;
    std::uint64_t tx_attempts_total = 0, tx_sent_total = 0, tx_failed_total = 0,
                  tx_complete_bytes_total = 0;
    std::uint64_t attempted_window = 0, sent_window = 0, not_sent_window = 0,
                  pending_after_drain = 0, bytes_sent_window = 0;
    std::size_t latency_samples = 0, latency_capacity = 0;
    bool latency_sampling_truncated = false;
    std::optional<double> latency_p50_ms, latency_p95_ms, latency_max_ms;
};

class ClientMetrics {
public:
    using TimeFn = std::function<MonoNs()>;
    ClientMetrics(MonoNs warmup, MonoNs duration, MonoNs drain, std::size_t sample_capacity, TimeFn clock);
    MonoNs connected();
    MonoNs stopped();
    void received(MonoNs at);
    TxTag attempt(MonoNs at);
    void completed(const SendCompletion& result);
    MeasurementSnapshot snapshot(bool quantiles = false) const;
private:
    bool in_window(MonoNs at) const;
    MonoNs cutoff() const;
    const MonoNs warmup_, duration_, drain_;
    const std::size_t capacity_;
    const TimeFn clock_;
    mutable std::mutex mutex_;
    MeasurementSnapshot state_;
    std::uint64_t next_tx_id_ = 1, timely_terminal_ = 0;
    std::vector<MonoNs> latency_;
};

struct ProcessSample {
    std::optional<double> cpu_percent;
    std::optional<std::uint64_t> rss_bytes;
};
class ProcessProbe {
public:
    ProcessProbe();
    ProcessSample sample();
    unsigned logical_cpus() const { return cores_; }
    static const char* platform();
private:
    unsigned cores_ = 0;
    std::optional<std::uint64_t> previous_cpu_;
    std::uint64_t previous_wall_ = 0;
};
} // namespace cclient
