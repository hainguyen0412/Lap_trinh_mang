#include "client_metrics.hpp"
#include <cmath>
#include <cstdio>
#include <stdexcept>

using namespace cclient;
constexpr MonoNs S = 1000000000ULL, MS = 1000000ULL;
namespace {
void check(bool value, const char* message) { if (!value) throw std::runtime_error(message); }
void equal(double actual, double expected) { check(std::abs(actual - expected) < 1e-8, "Unexpected measured value"); }

void rx_boundaries() {
    MonoNs now = 5 * S;
    ClientMetrics metrics(S, 2 * S, 200 * MS, 4, [&] { return now; });
    metrics.connected();
    for (auto at : {6 * S - 1, 6 * S, 7500 * MS, 8 * S}) metrics.received(at);
    now = 9 * S; metrics.stopped();
    const auto value = metrics.snapshot(true);
    check(value.valid_total == 4 && value.valid_window == 2, "Window must include start and exclude end");
    check(value.delivery_cutoff_ns == 8200 * MS, "Drain cutoff must follow window end, not a later shutdown");
    equal(value.measurement_s, 2);
}

void early_stops() {
    MonoNs now = 0;
    ClientMetrics before(S, 5 * S, 0, 0, [&] { return now; });
    before.connected(); now = 500 * MS; before.stopped();
    equal(before.snapshot().measurement_s, 0);
    now = 0;
    ClientMetrics during(S, 5 * S, 0, 0, [&] { return now; });
    during.connected(); now = 2250 * MS; during.stopped();
    equal(during.snapshot().measurement_s, 1.25);
}

void separate_directions_and_cutoff() {
    MonoNs now = 0;
    ClientMetrics metrics(S, S, 300 * MS, 8, [&] { return now; });
    metrics.connected();
    auto warmup = metrics.attempt(500 * MS);
    auto sent = metrics.attempt(S);
    auto rejected = metrics.attempt(1500 * MS);
    auto late = metrics.attempt(1800 * MS);
    auto post = metrics.attempt(2 * S);
    metrics.completed({warmup, 1100 * MS, 10, mavlink_gateway::DeliveryStatus::SentToSocket});
    metrics.completed({sent, 1010 * MS, 10, mavlink_gateway::DeliveryStatus::SentToSocket});
    metrics.completed({rejected, 1500 * MS, 10, mavlink_gateway::DeliveryStatus::ClientQueueFull});
    metrics.completed({late, 2310 * MS, 10, mavlink_gateway::DeliveryStatus::SentToSocket});
    metrics.completed({post, 2400 * MS, 10, mavlink_gateway::DeliveryStatus::SentToSocket});
    now = 3 * S; metrics.stopped();
    const auto value = metrics.snapshot(true);
    check(value.valid_window == 0 && value.attempted_window == 3, "RX and TX must have separate denominators");
    check(value.sent_window == 1 && value.not_sent_window == 2 && value.pending_after_drain == 1,
          "Late completion or queue rejection was counted as timely delivery");
    check(value.tx_sent_total == 4 && value.tx_failed_total == 1 && value.bytes_sent_window == 10,
          "Cumulative TX and window TX were mixed");
    check(value.latency_samples == 1, "Only timely successful attempts should produce latency samples");
    equal(*value.latency_p95_ms, 10);
}

void early_stop_cutoff() {
    MonoNs now = 0;
    ClientMetrics metrics(0, 10 * S, 100 * MS, 4, [&] { return now; });
    metrics.connected();
    const auto tag = metrics.attempt(100 * MS);
    now = 200 * MS; metrics.stopped();
    metrics.completed({tag, 350 * MS, 50, mavlink_gateway::DeliveryStatus::SentToSocket});
    const auto value = metrics.snapshot(true);
    check(value.delivery_cutoff_ns == 300 * MS && value.sent_window == 0 && value.pending_after_drain == 1,
          "Early stop did not shorten the delivery deadline");
    equal(value.measurement_s, 0.2);
}

void bounded_quantiles() {
    MonoNs now = 0;
    ClientMetrics metrics(0, S, 0, 2, [&] { return now; });
    metrics.connected();
    for (const auto sample : {3U, 1U, 9U}) {
        const auto tag = metrics.attempt(10 * MS);
        metrics.completed({tag, (10 + sample) * MS, 20, mavlink_gateway::DeliveryStatus::SentToSocket});
    }
    now = S; metrics.stopped();
    const auto value = metrics.snapshot(true);
    check(value.sent_window == 3 && value.latency_samples == 2 && value.latency_sampling_truncated,
          "Sample cap must not truncate delivery counters");
    equal(*value.latency_p50_ms, 1); equal(*value.latency_p95_ms, 3); equal(*value.latency_max_ms, 3);
}

void zero_window() {
    MonoNs now = 0;
    ClientMetrics metrics(0, 0, 0, 0, [&] { return now; });
    check(!metrics.snapshot().connected, "Unconnected client reported a connection");
    metrics.connected(); metrics.received(0);
    const auto tag = metrics.attempt(0);
    metrics.completed({tag, 0, 20, mavlink_gateway::DeliveryStatus::SentToSocket});
    now = S; metrics.stopped();
    const auto value = metrics.snapshot(true);
    check(value.valid_total == 1 && value.valid_window == 0 && value.attempted_window == 0,
          "Zero-duration window must stay empty");
    check(!value.latency_p50_ms && !value.latency_p95_ms && !value.latency_max_ms,
          "Missing latency must be blank, not zero");
}
} // namespace

int main() {
    const struct { const char* name; void (*run)(); } cases[] = {
        {"rx_boundaries", rx_boundaries}, {"early_stops", early_stops},
        {"separate_directions_and_cutoff", separate_directions_and_cutoff},
        {"early_stop_cutoff", early_stop_cutoff}, {"bounded_quantiles", bounded_quantiles}, {"zero_window", zero_window},
    };
    unsigned passed = 0;
    for (const auto& test : cases) {
        try { test.run(); ++passed; std::printf("PASS %s\n", test.name); }
        catch (const std::exception& error) { std::printf("FAIL %s: %s\n", test.name, error.what()); }
    }
    std::printf("%u/6 measurement tests passed.\n", passed);
    return passed == 6 ? 0 : 1;
}
