#pragma once
#include "command_manager.hpp"
#include "mavlink_gateway/config.hpp"
#include <filesystem>

namespace cclient {
struct ClientOptions {
    mavlink_gateway::Config shared;
    CommandConfig command;
    std::filesystem::path project_root, config_path;
    std::string host, run_id, name, resolved_config_json;
    std::uint16_t port = 0;
    unsigned heartbeat_interval_ms = 1000;
    mavlink_gateway::MeasurementMode mode = mavlink_gateway::MeasurementMode::B1;
    bool controller = false, verbose = false, help = false, print_config = false;
};

ClientOptions load_client_options(int argc, char** argv);
const char* client_usage();
} // namespace cclient
