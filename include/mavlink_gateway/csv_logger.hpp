#pragma once
#include "processing.hpp"
#include <filesystem>

namespace mavlink_gateway {
enum class LogTable { Frames, Issues, Delivery, Commands, Events, Metrics, ClientSummary };
struct LoggerOptions {
    std::filesystem::path root_directory{"output/runs"};
    std::string run_id;
    std::string component{"gateway"};
    MeasurementMode mode{MeasurementMode::B1};
    std::uint64_t seed{20261111};
    std::size_t queue_records{10000};
    std::size_t max_record_bytes{16384};
    std::uint64_t max_file_bytes{104857600};
    std::uint64_t max_run_bytes{536870912};
    std::size_t flush_interval_ms{1000};
    std::size_t shutdown_drain_ms{2000};
    bool write_control_files{true};
    std::uint64_t reserved_control_bytes{4096};
    std::string scope{"component_b"};
    std::map<std::string,std::string> metadata;
};
struct LoggerStats {
    std::uint64_t submitted{};
    std::uint64_t written{};
    std::uint64_t dropped{};
    std::uint64_t disabled{};
    bool log_truncated{};
    std::string last_error;
};
class CsvLogger {
public:
    explicit CsvLogger(LoggerOptions options);
    ~CsvLogger();
    CsvLogger(const CsvLogger&)=delete;
    CsvLogger& operator=(const CsvLogger&)=delete;
    void record_frame(const FrameRecord& frame);
    void record_issue(const ParseIssue& issue);
    void record_delivery(const DeliveryRecord& delivery);
    // Fields exclude run_id/component, which the logger adds. Unknown column names throw.
    void record_fields(LogTable table,const std::map<std::string,std::string>& fields);
    void record_row(LogTable table,const std::vector<std::string>& fields);
    void set_summary(const std::map<std::string,std::string>& fields);
    LoggerStats stats() const;
    void close(); // Drain bounded queue, join worker, write summary and final metadata.
    std::filesystem::path directory() const;
private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
} // namespace mavlink_gateway
