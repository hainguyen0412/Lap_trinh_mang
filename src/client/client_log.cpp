#include "client_log.hpp"
#include <nlohmann/json.hpp>
#include <cstdio>
#include <fstream>
#if __has_include("client_build.hpp")
#include "client_build.hpp"
#endif
#ifndef C_CLIENT_SOURCE_SHA256
#define C_CLIENT_SOURCE_SHA256 "unavailable"
#endif

namespace cclient {
using namespace mavlink_gateway;
using Json = nlohmann::json;
namespace {
[[maybe_unused]] constexpr std::size_t report_budget = 8192;
[[maybe_unused]] std::string number(const std::optional<double>& value) { return value ? std::to_string(*value) : ""; }
[[maybe_unused]] Json optional(const std::optional<double>& value) { return value ? Json(*value) : Json(nullptr); }
}

ClientLog::ClientLog(const ClientOptions& options, ClientMetrics::TimeFn clock)
    : options_(options), clock_(std::move(clock)) {
#ifndef C_NO_LOGGER
    if (options.run_id.empty()) return;
    LoggerOptions o;
    o.root_directory = options.shared.output_root; o.run_id = options.run_id; o.component = options.name;
    o.mode = options.mode; o.seed = options.shared.seed; o.scope = "client_c";
    o.queue_records = options.shared.logger_records; o.max_record_bytes = options.shared.log_record_bytes;
    o.max_file_bytes = options.shared.log_file_bytes; o.max_run_bytes = options.shared.log_run_bytes;
    o.flush_interval_ms = options.shared.log_flush_ms; o.shutdown_drain_ms = options.shared.log_drain_ms;
    const auto snapshot_bytes = options.resolved_config_json.size() + 1;
    if (snapshot_bytes > o.max_file_bytes || snapshot_bytes + report_budget + 8192 > o.max_run_bytes)
        throw std::invalid_argument("Logging budget is too small for client control files");
    o.reserved_control_bytes = 4096 + snapshot_bytes + report_budget;
    o.metadata = {{"config_path", options.config_path.string()}, {"resolved_config_file", "resolved_config.json"},
                  {"measurement_file", "measurement.json"}, {"clock_policy", "steady_clock_ns_since_process_origin"},
                  {"measurement_origin", "tcp_connected"}, {"delivery_scope", "client_uplink"}};
    logger_ = std::make_unique<CsvLogger>(o);
    std::ofstream snapshot(logger_->directory() / "resolved_config.json", std::ios::binary | std::ios::trunc);
    snapshot.exceptions(std::ios::failbit | std::ios::badbit);
    snapshot << options.resolved_config_json << '\n'; snapshot.close();
    std::printf("Log: %s\n", logger_->directory().string().c_str());
#endif
}

void ClientLog::error(const char* message) noexcept {
    if (!failed_.exchange(true)) std::fprintf(stderr, "Client log error: %s\n", message);
}

void ClientLog::frame([[maybe_unused]] const FrameRecord& frame) noexcept {
#ifndef C_NO_LOGGER
    try { if (logger_) logger_->record_frame(frame); } catch (const std::exception& e) { error(e.what()); }
#endif
}
void ClientLog::issue([[maybe_unused]] const ParseIssue& issue) noexcept {
#ifndef C_NO_LOGGER
    try { if (logger_) logger_->record_issue(issue); } catch (const std::exception& e) { error(e.what()); }
#endif
}
void ClientLog::event([[maybe_unused]] const char* name, [[maybe_unused]] const std::string& detail) noexcept {
#ifndef C_NO_LOGGER
    try { if (logger_) logger_->record_fields(LogTable::Events,
        {{"event_ns", std::to_string(clock_())}, {"session_id", "1"}, {"source_id", "1"}, {"event", name}, {"detail", detail}}); }
    catch (const std::exception& e) { error(e.what()); }
#endif
}
void ClientLog::command([[maybe_unused]] const CommandEvent& e) noexcept {
#ifndef C_NO_LOGGER
    try {
        if (!logger_) return;
        std::map<std::string, std::string> fields{{"request_id", std::to_string(e.request_id)}, {"session_id", "1"},
            {"command", std::to_string(e.command)}, {"target_system", std::to_string(e.target_system)},
            {"target_component", std::to_string(e.target_component)}, {"attempt", std::to_string(e.attempt)},
            {"event_ns", std::to_string(e.at_ns)}, {"event", e.event}};
        if (e.result) fields["result"] = std::to_string(*e.result);
        if (e.progress) fields["progress"] = std::to_string(*e.progress);
        logger_->record_fields(LogTable::Commands, fields);
    } catch (const std::exception& e) { error(e.what()); }
#endif
}

void ClientLog::tx_attempt([[maybe_unused]] TxTag tag, [[maybe_unused]] std::size_t bytes) noexcept {
#ifndef C_NO_LOGGER
    try { if (logger_ && options_.mode == MeasurementMode::B2) logger_->record_fields(LogTable::Events,
        {{"event_ns", std::to_string(tag.queued_at_ns)}, {"session_id", "1"}, {"source_id", "1"},
         {"event", "uplink_attempt"}, {"detail", Json{{"tx_id", tag.id}, {"raw_len", bytes}}.dump()}}); }
    catch (const std::exception& e) { error(e.what()); }
#endif
}
void ClientLog::tx_completed([[maybe_unused]] const SendCompletion& result) noexcept {
#ifndef C_NO_LOGGER
    try { if (logger_ && options_.mode == MeasurementMode::B2) logger_->record_fields(LogTable::Events,
        {{"event_ns", std::to_string(result.completed_at_ns)}, {"session_id", "1"}, {"source_id", "1"},
         {"event", std::string("uplink_") + delivery_name(result.status)},
         {"detail", Json{{"tx_id", result.tag.id}, {"queued_ns", result.tag.queued_at_ns}, {"raw_len", result.frame_bytes}}.dump()}}); }
    catch (const std::exception& e) { error(e.what()); }
#endif
}

void ClientLog::metrics([[maybe_unused]] MonoNs at, [[maybe_unused]] const MeasurementSnapshot& c,
                        [[maybe_unused]] const InspectorSnapshot& inspector, [[maybe_unused]] const SendQueueSnapshot& tx,
                        [[maybe_unused]] const ProcessSample& process, [[maybe_unused]] bool active) noexcept {
#ifndef C_NO_LOGGER
    try {
        if (!logger_) return;
        std::map<std::string, std::string> fields{{"sample_ns", std::to_string(at)}, {"valid_total", std::to_string(c.valid_total)},
            {"client_queue_drop", std::to_string(tx.queue_full)}, {"log_drop", std::to_string(logger_->stats().dropped)},
            {"active_clients", active ? "1" : "0"}, {"max_client_queue_frames", std::to_string(tx.queued_frames)}};
        if (previous_sample_ && at > previous_sample_) {
            const double seconds = double(at - previous_sample_) / 1e9;
            fields["rx_frames_per_s"] = std::to_string(double(c.valid_total - previous_rx_) / seconds);
            fields["tx_bytes_per_s"] = std::to_string(double(tx.socket_bytes - previous_tx_bytes_) / seconds);
        }
        previous_sample_ = at; previous_rx_ = c.valid_total; previous_tx_bytes_ = tx.socket_bytes;
        if (process.cpu_percent) fields["cpu_percent"] = std::to_string(*process.cpu_percent);
        if (process.rss_bytes) fields["rss_bytes"] = std::to_string(*process.rss_bytes);
        if (options_.mode != MeasurementMode::B0) {
            std::uint64_t gap = 0, repeated = 0, ambiguous = 0;
            for (const auto& source : inspector.sources) {
                gap += source.estimated_missing; repeated += source.repeated_seq; ambiguous += source.ambiguous_seq;
            }
            fields.insert({{"bad_crc_total", std::to_string(inspector.bad_crc_candidates)},
                {"unsupported_total", std::to_string(inspector.unsupported_candidates)},
                {"truncated_datagrams", std::to_string(inspector.truncated_datagrams)},
                {"noise_bytes", std::to_string(inspector.discarded_noise_bytes)},
                {"seq_gap_total", std::to_string(gap)}, {"seq_repeat_total", std::to_string(repeated)},
                {"seq_ambiguous_total", std::to_string(ambiguous)}});
        }
        logger_->record_fields(LogTable::Metrics, fields);
    } catch (const std::exception& e) { error(e.what()); }
#endif
}

int ClientLog::finish(const MeasurementSnapshot& c, const SendQueueSnapshot& tx,
                       [[maybe_unused]] unsigned logical_cpus, [[maybe_unused]] const std::string& peer,
                       [[maybe_unused]] bool drained, bool sampler_failed,
                       [[maybe_unused]] std::size_t shutdown_pending) noexcept {
    std::printf("Measurement: %.6fs; received=%llu; uplink_attempted=%llu sent_to_socket=%llu not_sent=%llu pending_after_drain=%llu\n",
        c.measurement_s, (unsigned long long)c.valid_window, (unsigned long long)c.attempted_window,
        (unsigned long long)c.sent_window, (unsigned long long)c.not_sent_window, (unsigned long long)c.pending_after_drain);
#ifndef C_NO_LOGGER
    try {
        if (logger_) {
            logger_->set_summary({{"clients", c.connected ? "1" : "0"}, {"measurement_s", std::to_string(c.measurement_s)},
                {"valid_window", std::to_string(c.valid_window)}, {"sent_window", std::to_string(c.sent_window)},
                {"not_sent_window", std::to_string(c.not_sent_window)}, {"pending_after_drain", std::to_string(c.pending_after_drain)},
                {"latency_samples", std::to_string(c.latency_samples)}, {"latency_p50_ms", number(c.latency_p50_ms)},
                {"latency_p95_ms", number(c.latency_p95_ms)}, {"latency_max_ms", number(c.latency_max_ms)}});
            logger_->record_fields(LogTable::ClientSummary, {{"client_id", "1"}, {"role", options_.controller ? "controller" : "observer"},
                {"attempted_window", std::to_string(c.attempted_window)}, {"sent_window", std::to_string(c.sent_window)},
                {"not_sent_window", std::to_string(c.not_sent_window)}, {"bytes_sent_window", std::to_string(c.bytes_sent_window)},
                {"latency_samples", std::to_string(c.latency_samples)}, {"latency_p95_ms", number(c.latency_p95_ms)},
                {"latency_max_ms", number(c.latency_max_ms)}, {"latency_sampling_truncated", c.latency_sampling_truncated ? "1" : "0"}});
            logger_->close();
            const auto stats = logger_->stats();
            if (!stats.last_error.empty()) error(stats.last_error.c_str());
            Json report{{"schema_version", 1}, {"run_id", options_.run_id}, {"component", options_.name},
                {"platform", ProcessProbe::platform()}, {"logical_cpus", logical_cpus}, {"compiler", __VERSION__},
                {"program_source_sha256", C_CLIENT_SOURCE_SHA256}, {"program_commit", nullptr}, {"px4_version", nullptr},
                {"tcp_peer", peer.empty() ? Json(nullptr) : Json(peer)}, {"clock_policy", "steady_clock_ns_since_process_origin"},
                {"measurement_origin", "tcp_connected"}, {"connected_at_ns", c.connected_at_ns},
                {"window_start_ns", c.window_start_ns}, {"window_end_ns", c.window_end_ns}, {"client_stopped_ns", c.stopped_at_ns},
                {"delivery_cutoff_ns", c.delivery_cutoff_ns}, {"measurement_s", c.measurement_s},
                {"valid_total", c.valid_total}, {"valid_window", c.valid_window},
                {"uplink", {{"attempts_total", c.tx_attempts_total}, {"sent_total", c.tx_sent_total}, {"failed_total", c.tx_failed_total},
                    {"socket_bytes_total", tx.socket_bytes}, {"attempted_window", c.attempted_window}, {"sent_window", c.sent_window},
                    {"not_sent_window", c.not_sent_window}, {"pending_after_drain", c.pending_after_drain}, {"bytes_sent_window", c.bytes_sent_window}}},
                {"latency", {{"scope", "client_uplink_enqueue_to_socket_completion"}, {"samples", c.latency_samples},
                    {"capacity", c.latency_capacity}, {"sampling", "first_N_timely_successes"}, {"sampling_truncated", c.latency_sampling_truncated},
                    {"p50_ms", optional(c.latency_p50_ms)}, {"p95_ms", optional(c.latency_p95_ms)}, {"max_ms", optional(c.latency_max_ms)}}},
                {"shutdown_drained", drained}, {"shutdown_pending_before_discard", shutdown_pending},
                {"tx_queue_peak_frames", tx.peak_queued_frames}, {"tx_queue_full", tx.queue_full}, {"tx_socket_failed", tx.failed},
                {"tx_observer_failed", tx.observer_failed}, {"metrics_sampler_failed", sampler_failed},
                {"log_drop", stats.dropped}, {"log_truncated", stats.log_truncated},
                {"log_error", stats.last_error}, {"client_log_failed", failed_.load()},
                {"definitions", {{"valid_window", "accepted downlink frames in measurement window"},
                    {"sent_window", "uplink attempts in window fully accepted by socket before delivery cutoff"},
                    {"not_sent_window", "uplink attempted_window minus sent_window; unrelated to downlink count"},
                    {"pending_after_drain", "window attempts without a terminal result before delivery cutoff"},
                    {"cpu_percent", "0-100 percent of all logical CPUs"}, {"tx_bytes_per_s", "OS send-accepted bytes including partial frames"},
                    {"command_queued", "queue admission; ACK timeout starts here"}, {"peer_receipt_confirmed", false}}}};
            const auto text = report.dump(2) + '\n';
            if (text.size() > report_budget || text.size() > options_.shared.log_file_bytes)
                throw std::runtime_error("measurement.json exceeds control-file budget");
            std::ofstream file(logger_->directory() / "measurement.json", std::ios::binary | std::ios::trunc);
            file.exceptions(std::ios::failbit | std::ios::badbit); file << text; file.close();
            std::printf("Log: written=%llu dropped=%llu truncated=%d\n", (unsigned long long)stats.written,
                        (unsigned long long)stats.dropped, stats.log_truncated ? 1 : 0);
        }
    } catch (const std::exception& e) { error(e.what()); }
#else
    (void)tx;
#endif
    return failed_ || tx.observer_failed || sampler_failed ? 1 : 0;
}
} // namespace cclient
