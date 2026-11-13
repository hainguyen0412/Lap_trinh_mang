#include "client_config.hpp"
#include <nlohmann/json.hpp>
#include <algorithm>
#include <fstream>
#include <map>
#include <set>
#include <stdexcept>
#ifdef _WIN32
#include <windows.h>
#endif

namespace cclient {
namespace {
using Json = nlohmann::json;
namespace fs = std::filesystem;

std::uint64_t integer(const std::string& text, std::uint64_t minimum,
                      std::uint64_t maximum, const std::string& option) {
    if (text.empty() || !std::all_of(text.begin(), text.end(), [](unsigned char c) { return c >= '0' && c <= '9'; }))
        throw std::invalid_argument("Expected decimal integer for " + option);
    std::uint64_t value;
    try { value = std::stoull(text); }
    catch (const std::out_of_range&) { throw std::invalid_argument("Out of range: " + option); }
    if (value < minimum || value > maximum) throw std::invalid_argument("Out of range: " + option);
    return value;
}

void identifier(const std::string& value, const std::string& option) {
    if (value.empty() || value.size() > 80 ||
        value.find_first_not_of("abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789_-") != std::string::npos)
        throw std::invalid_argument(option + ": use 1-80 letters, digits, _ or -");
}

fs::path project_root(const char* argv0, const fs::path& config) {
    fs::path executable = fs::absolute(argv0);
#ifdef _WIN32
    wchar_t buffer[32768]{};
    const auto length = GetModuleFileNameW(nullptr, buffer, 32768);
    if (length && length < 32768) executable = fs::path(buffer);
#endif
    for (auto start : {executable.parent_path(), fs::current_path(), config.parent_path()}) {
        while (!start.empty()) {
            if (fs::is_regular_file(start / "include/mavlink_gateway/contracts.hpp") &&
                fs::is_regular_file(start / "config/gateway.example.json")) return start.lexically_normal();
            const auto parent = start.parent_path();
            if (parent == start) break;
            start = parent;
        }
    }
    throw std::runtime_error("Cannot locate shared project root; keep the executable inside the project");
}
} // namespace

ClientOptions load_client_options(int argc, char** argv) {
    ClientOptions options;
    std::map<std::string, std::string> cli;
    const std::set<std::string> valued{
        "--config", "--role", "--host", "--port", "--px4-sys", "--px4-comp", "--gcs-sys", "--gcs-comp",
        "--heartbeat-ms", "--ack-timeout-ms", "--max-attempts", "--progress-idle-ms", "--progress-total-ms",
        "--mode", "--log-root", "--run-id", "--name"
    };
    std::vector<std::string> positional;
    for (int i = 1; i < argc; ++i) {
        const std::string key = argv[i];
        if (key == "--help") { options.help = true; return options; }
        if (key == "--verbose") {
            if (options.verbose) throw std::invalid_argument("Duplicate option: --verbose");
            options.verbose = true;
        } else if (key == "--print-config") {
            if (options.print_config) throw std::invalid_argument("Duplicate option: --print-config");
            options.print_config = true;
        } else if (valued.count(key)) {
            if (i + 1 >= argc) throw std::invalid_argument("Missing value: " + key);
            const std::string value = argv[++i];
            if (value.empty() || value.rfind("--", 0) == 0) throw std::invalid_argument("Missing value: " + key);
            if (!cli.emplace(key, value).second) throw std::invalid_argument("Duplicate option: " + key);
        } else if (!key.empty() && key[0] == '-') {
            throw std::invalid_argument("Unknown option: " + key);
        } else {
            if (key.empty() || positional.size() == 2) throw std::invalid_argument("Expected at most host and port");
            positional.push_back(key);
        }
    }
    if (!positional.empty() && !cli.emplace("--host", positional[0]).second)
        throw std::invalid_argument("Host specified both positionally and with --host");
    if (positional.size() == 2 && !cli.emplace("--port", positional[1]).second)
        throw std::invalid_argument("Port specified both positionally and with --port");
    if (options.verbose) cli.emplace("--verbose", "true");
    const fs::path requested = cli.count("--config") ? fs::absolute(cli.at("--config")) : fs::path{};
    options.project_root = project_root(argv[0], requested);
    options.config_path = (requested.empty() ? options.project_root / "config/gateway.example.json" : requested).lexically_normal();

    // Reuse A's complete shared-schema validator; do not maintain a second JSON schema.
    options.shared = mavlink_gateway::load_config(options.config_path.string());
    const auto source = Json::parse(options.shared.resolved_config_json);
    options.host = source.at("client").at("gateway_host").get<std::string>();
    options.heartbeat_interval_ms = source.at("client").at("heartbeat_interval_ms").get<unsigned>();
    const auto& commands = source.at("commands");
    options.command.px4_sysid = options.shared.px4_sysid;
    options.command.px4_compid = options.shared.px4_compid;
    options.command.gcs_sysid = options.shared.gcs_sysid;
    options.command.gcs_compid = options.shared.gcs_compid;
    options.command.ack_timeout_ns = commands.at("ack_timeout_ms").get<MonoNs>() * 1000000ULL;
    options.command.max_sends = commands.at("max_total_attempts").get<unsigned>();
    options.command.progress_idle_ns = commands.at("in_progress_idle_timeout_ms").get<MonoNs>() * 1000000ULL;
    options.command.progress_total_ns = commands.at("in_progress_total_timeout_ms").get<MonoNs>() * 1000000ULL;

    if (cli.count("--role")) {
        const auto& role = cli.at("--role");
        if (role != "observer" && role != "controller") throw std::invalid_argument("--role must be observer/controller");
        options.controller = role == "controller";
    }
    options.port = options.controller ? options.shared.controller_port : options.shared.observer_port;
    if (cli.count("--host")) options.host = cli.at("--host");
    if (cli.count("--port")) options.port = static_cast<std::uint16_t>(integer(cli.at("--port"), 1, 65535, "--port"));
    auto id = [&](const char* key, std::uint8_t& destination) {
        if (cli.count(key)) destination = static_cast<std::uint8_t>(integer(cli.at(key), 1, 255, key));
    };
    id("--px4-sys", options.command.px4_sysid); id("--px4-comp", options.command.px4_compid);
    id("--gcs-sys", options.command.gcs_sysid); id("--gcs-comp", options.command.gcs_compid);
    auto timeout = [&](const char* key, MonoNs& destination, std::uint64_t maximum) {
        if (cli.count(key)) destination = integer(cli.at(key), 1, maximum, key) * 1000000ULL;
    };
    timeout("--ack-timeout-ms", options.command.ack_timeout_ns, 60000);
    timeout("--progress-idle-ms", options.command.progress_idle_ns, 600000);
    timeout("--progress-total-ms", options.command.progress_total_ns, 600000);
    if (options.command.progress_total_ns < options.command.progress_idle_ns)
        throw std::invalid_argument("Progress total timeout must be >= idle timeout");
    if (cli.count("--heartbeat-ms")) options.heartbeat_interval_ms = static_cast<unsigned>(integer(cli.at("--heartbeat-ms"), 1, 60000, "--heartbeat-ms"));
    if (cli.count("--max-attempts")) options.command.max_sends = static_cast<unsigned>(integer(cli.at("--max-attempts"), 1, 3, "--max-attempts"));
    const auto mode = cli.count("--mode") ? cli.at("--mode") : options.shared.mode;
    if (mode != "B0" && mode != "B1" && mode != "B2") throw std::invalid_argument("--mode must be B0/B1/B2");
    options.mode = mode == "B0" ? mavlink_gateway::MeasurementMode::B0 : mode == "B1" ? mavlink_gateway::MeasurementMode::B1 : mavlink_gateway::MeasurementMode::B2;
    options.shared.mode = mode;
    if (cli.count("--log-root")) options.shared.output_root = cli.at("--log-root");
    if (fs::path(options.shared.output_root).is_relative())
        options.shared.output_root = (options.project_root / options.shared.output_root).lexically_normal().string();
    else options.shared.output_root = fs::path(options.shared.output_root).lexically_normal().string();
    if (cli.count("--run-id")) { options.run_id = cli.at("--run-id"); identifier(options.run_id, "--run-id"); }
    options.name = cli.count("--name") ? cli.at("--name") : options.controller ? "controller" : "observer_01";
    identifier(options.name, "--name");
    bool logging_enabled = !options.run_id.empty();
#ifdef C_NO_LOGGER
    logging_enabled = false;
#endif

    Json effective{
        {"connection", {{"role", options.controller ? "controller" : "observer"}, {"gateway_host", options.host}, {"port", options.port}}},
        {"display", {{"verbose", options.verbose}}},
        {"client", {{"gcs_sysid", options.command.gcs_sysid}, {"gcs_compid", options.command.gcs_compid}, {"heartbeat_interval_ms", options.heartbeat_interval_ms}}},
        {"px4", {{"expected_sysid", options.command.px4_sysid}, {"expected_compid", options.command.px4_compid}}},
        {"commands", {{"allowed_command_ids", options.shared.allowed_commands}, {"max_pending", 1}, {"ack_timeout_ms", options.command.ack_timeout_ns / 1000000ULL},
                       {"max_total_attempts", options.command.max_sends}, {"in_progress_idle_timeout_ms", options.command.progress_idle_ns / 1000000ULL},
                       {"in_progress_total_timeout_ms", options.command.progress_total_ns / 1000000ULL}}},
        {"limits", {{"tcp_parser_buffer_bytes", options.shared.tcp_buffer_bytes}, {"client_send_queue_frames", options.shared.client_queue_frames},
                     {"client_send_queue_bytes", options.shared.client_queue_bytes}, {"logger_queue_records", options.shared.logger_records}}},
        {"measurement", {{"mode", mode}, {"seed", options.shared.seed}, {"origin", "tcp_connected"},
            {"warmup_seconds", options.shared.warmup_seconds}, {"duration_seconds", options.shared.measurement_seconds},
            {"snapshot_interval_ms", options.shared.snapshot_interval_ms}, {"drain_timeout_ms", options.shared.drain_ms},
            {"latency_sample_capacity", options.controller ? 1000000 : 0}}},
        {"logging", {{"enabled", logging_enabled}, {"root_directory", options.shared.output_root}, {"run_id", options.run_id}, {"component", options.name},
                      {"flush_interval_ms", options.shared.log_flush_ms}, {"shutdown_drain_ms", options.shared.log_drain_ms},
                      {"max_record_bytes", options.shared.log_record_bytes}, {"max_file_bytes", options.shared.log_file_bytes}, {"max_component_run_bytes", options.shared.log_run_bytes}}}
    };
    options.resolved_config_json = Json{{"schema_version", 1}, {"config_path", options.config_path.string()},
        {"shared_config", source}, {"effective_client", effective}, {"cli_overrides", cli}}.dump(2);
    return options;
}

const char* client_usage() {
    return "c_client [host] [port] [--config FILE] [--role observer|controller]\n"
           "  [--host HOST] [--port N] [--px4-sys N] [--px4-comp N] [--gcs-sys N] [--gcs-comp N]\n"
           "  [--heartbeat-ms N] [--ack-timeout-ms N] [--max-attempts N]\n"
           "  [--progress-idle-ms N] [--progress-total-ms N] [--mode B0|B1|B2]\n"
           "  [--log-root DIR] [--run-id ID] [--name COMPONENT] [--verbose] [--print-config] [--help]";
}
} // namespace cclient
