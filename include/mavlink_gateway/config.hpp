#pragma once
#include "mavlink_gateway/contracts.hpp"
#include <string>

namespace mavlink_gateway {
struct Config {
    std::string udp_bind_host="0.0.0.0",observer_bind_host="0.0.0.0",controller_bind_host="0.0.0.0";
    std::string run_id,output_root="output/runs",resolved_config_json;
    std::string stop_file, mode = "B1";
    std::uint16_t udp_port = 14550, observer_port = 5760, controller_port = 5761;
    std::uint8_t px4_sysid = 1, px4_compid = 1, gcs_sysid = 255, gcs_compid = 190;
    std::size_t max_observers = 9, max_udp_candidates = 16;
    std::size_t udp_queue_datagrams = 4096, client_queue_frames = 1024, client_queue_bytes = 1048576;
    std::size_t udp_uplink_frames = 256, tcp_buffer_bytes = 65536, logger_records = 10000;
    std::size_t log_file_bytes = 100 * 1024 * 1024, log_run_bytes = 512 * 1024 * 1024;
    std::size_t log_record_bytes=16384;
    int log_flush_ms=1000,log_drain_ms=2000,snapshot_interval_ms=1000;
    bool reject_commands_when_stale=true;
    std::vector<std::uint16_t> allowed_commands{400,22,21};
    int heartbeat_stale_ms = 3000, drain_ms = 5000, send_timeout_ms = 5000;
    int socket_send_buffer_bytes = 0, send_chunk_bytes = 0;
    int run_seconds = 0, warmup_seconds = 10, measurement_seconds = 60;
    std::uint32_t seed = 20261111;
};
Config load_config(const std::string& path);
} // namespace mavlink_gateway
