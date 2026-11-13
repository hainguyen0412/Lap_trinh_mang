#pragma once
#include "client_config.hpp"
#include "client_metrics.hpp"
#include "send_queue.hpp"
#ifndef C_NO_LOGGER
#include "mavlink_gateway/csv_logger.hpp"
#endif
#include <atomic>
#include <memory>

namespace cclient {
class ClientLog {
public:
    ClientLog(const ClientOptions& options, ClientMetrics::TimeFn clock);
    void frame(const mavlink_gateway::FrameRecord& frame) noexcept;
    void issue(const mavlink_gateway::ParseIssue& issue) noexcept;
    void event(const char* name, const std::string& detail = "") noexcept;
    void command(const CommandEvent& event) noexcept;
    void tx_attempt(TxTag tag, std::size_t bytes) noexcept;
    void tx_completed(const SendCompletion& result) noexcept;
    void metrics(MonoNs at, const MeasurementSnapshot& counters,
                 const mavlink_gateway::InspectorSnapshot& inspector,
                 const SendQueueSnapshot& tx, const ProcessSample& process, bool active) noexcept;
    int finish(const MeasurementSnapshot& counters, const SendQueueSnapshot& tx,
               unsigned logical_cpus, const std::string& peer, bool drained, bool sampler_failed = false,
               std::size_t shutdown_pending = 0) noexcept;
private:
    void error(const char* message) noexcept;
    const ClientOptions options_;
    const ClientMetrics::TimeFn clock_;
    std::atomic<bool> failed_{false};
    MonoNs previous_sample_ = 0;
    std::uint64_t previous_rx_ = 0, previous_tx_bytes_ = 0;
#ifndef C_NO_LOGGER
    std::unique_ptr<mavlink_gateway::CsvLogger> logger_;
#endif
};
} // namespace cclient
