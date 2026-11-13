#include "client_metrics.hpp"
#include <algorithm>
#include <chrono>
#include <cmath>
#include <fstream>
#include <thread>
#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <psapi.h>
#else
#include <sys/resource.h>
#include <unistd.h>
#endif

namespace cclient {
ClientMetrics::ClientMetrics(MonoNs warmup, MonoNs duration, MonoNs drain,
                             std::size_t sample_capacity, TimeFn clock)
    : warmup_(warmup), duration_(duration), drain_(drain), capacity_(sample_capacity), clock_(std::move(clock)) {
    latency_.reserve(capacity_);
    state_.latency_capacity = capacity_;
}

MonoNs ClientMetrics::connected() {
    std::lock_guard<std::mutex> lock(mutex_);
    state_.connected = true;
    state_.connected_at_ns = clock_();
    state_.window_start_ns = state_.connected_at_ns + warmup_;
    state_.window_end_ns = state_.window_start_ns + duration_;
    return state_.connected_at_ns;
}

MonoNs ClientMetrics::stopped() {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!state_.stopped) { state_.stopped_at_ns = clock_(); state_.stopped = true; }
    return state_.stopped_at_ns;
}

bool ClientMetrics::in_window(MonoNs at) const {
    return state_.connected && at >= state_.window_start_ns && at < state_.window_end_ns &&
           (!state_.stopped || at < state_.stopped_at_ns);
}

MonoNs ClientMetrics::cutoff() const {
    if (!state_.connected) return 0;
    return (state_.stopped ? std::min(state_.window_end_ns, state_.stopped_at_ns) : state_.window_end_ns) + drain_;
}

void ClientMetrics::received(MonoNs at) {
    std::lock_guard<std::mutex> lock(mutex_);
    ++state_.valid_total;
    if (in_window(at)) ++state_.valid_window;
}

TxTag ClientMetrics::attempt(MonoNs at) {
    std::lock_guard<std::mutex> lock(mutex_);
    ++state_.tx_attempts_total;
    if (in_window(at)) ++state_.attempted_window;
    return {next_tx_id_++, at};
}

void ClientMetrics::completed(const SendCompletion& result) {
    std::lock_guard<std::mutex> lock(mutex_);
    const bool sent = result.status == mavlink_gateway::DeliveryStatus::SentToSocket;
    if (sent) { ++state_.tx_sent_total; state_.tx_complete_bytes_total += result.frame_bytes; }
    else ++state_.tx_failed_total;
    if (!in_window(result.tag.queued_at_ns) || result.completed_at_ns > cutoff()) return;
    ++timely_terminal_;
    if (!sent) return;
    ++state_.sent_window;
    state_.bytes_sent_window += result.frame_bytes;
    const auto elapsed = result.completed_at_ns >= result.tag.queued_at_ns ?
                         result.completed_at_ns - result.tag.queued_at_ns : 0;
    if (latency_.size() < capacity_) latency_.push_back(elapsed);
    else state_.latency_sampling_truncated = true;
}

MeasurementSnapshot ClientMetrics::snapshot(bool quantiles) const {
    std::vector<MonoNs> samples;
    MeasurementSnapshot result;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        result = state_;
        result.delivery_cutoff_ns = cutoff();
        if (state_.connected) {
            const auto until = std::min(state_.window_end_ns, state_.stopped ? state_.stopped_at_ns : clock_());
            if (until > state_.window_start_ns) result.measurement_s = double(until - state_.window_start_ns) / 1e9;
        }
        result.not_sent_window = state_.attempted_window - state_.sent_window;
        result.pending_after_drain = state_.attempted_window - timely_terminal_;
        result.latency_samples = latency_.size();
        if (quantiles) samples = latency_;
    }
    if (!samples.empty()) {
        std::sort(samples.begin(), samples.end());
        result.latency_p50_ms = samples[(samples.size() + 1) / 2 - 1] / 1e6;
        result.latency_p95_ms = samples[(95 * samples.size() + 99) / 100 - 1] / 1e6;
        result.latency_max_ms = samples.back() / 1e6;
    }
    return result;
}

namespace {
std::uint64_t wall_ns() {
    return static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count());
}
std::optional<std::uint64_t> cpu_ns() {
#ifdef _WIN32
    FILETIME created{}, exited{}, kernel{}, user{};
    if (!GetProcessTimes(GetCurrentProcess(), &created, &exited, &kernel, &user)) return std::nullopt;
    auto value = [](FILETIME time) { return (std::uint64_t(time.dwHighDateTime) << 32U) | time.dwLowDateTime; };
    return (value(kernel) + value(user)) * 100ULL;
#else
    rusage usage{};
    if (getrusage(RUSAGE_SELF, &usage) != 0) return std::nullopt;
    return std::uint64_t(usage.ru_utime.tv_sec + usage.ru_stime.tv_sec) * 1000000000ULL +
           std::uint64_t(usage.ru_utime.tv_usec + usage.ru_stime.tv_usec) * 1000ULL;
#endif
}
std::optional<std::uint64_t> rss_bytes() {
#ifdef _WIN32
    PROCESS_MEMORY_COUNTERS memory{};
    if (!GetProcessMemoryInfo(GetCurrentProcess(), &memory, sizeof memory)) return std::nullopt;
    return static_cast<std::uint64_t>(memory.WorkingSetSize);
#else
    std::ifstream stat("/proc/self/statm");
    std::uint64_t pages = 0, resident = 0;
    const auto page_size = sysconf(_SC_PAGESIZE);
    if (!(stat >> pages >> resident) || page_size <= 0) return std::nullopt;
    return resident * static_cast<std::uint64_t>(page_size);
#endif
}
} // namespace

ProcessProbe::ProcessProbe() {
#ifdef _WIN32
    cores_ = GetActiveProcessorCount(ALL_PROCESSOR_GROUPS);
#else
    cores_ = std::thread::hardware_concurrency();
#endif
    previous_cpu_ = cpu_ns();
    previous_wall_ = wall_ns();
}

ProcessSample ProcessProbe::sample() {
    ProcessSample result;
    result.rss_bytes = rss_bytes();
    const auto cpu = cpu_ns();
    const auto now = wall_ns();
    if (cpu && previous_cpu_ && *cpu >= *previous_cpu_ && now > previous_wall_ && cores_)
        result.cpu_percent = std::clamp(100.0 * double(*cpu - *previous_cpu_) / double(now - previous_wall_) / cores_, 0.0, 100.0);
    previous_cpu_ = cpu;
    previous_wall_ = now;
    return result;
}

const char* ProcessProbe::platform() {
#ifdef _WIN32
    return "Windows";
#else
    return "Linux";
#endif
}
} // namespace cclient
