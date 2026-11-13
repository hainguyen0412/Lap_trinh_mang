// Client C: nhan TCP tu gateway -> Parser -> Decoder -> Inspector -> in ra.
// --role controller: HEARTBEAT GCS + Command Manager use shared JSON settings.
//
// Cach dung:
//   c_client.exe [host] [port] [--role observer|controller] [--px4-sys 1] [--px4-comp 1]
//                [--config FILE] [--run-id ID] [--name ten_component] [--mode B0|B1|B2] [--verbose]
//   --help lists all explicit CLI overrides; --print-config resolves settings without connecting.
//   (port mac dinh: observer 5760, controller 5761)
// Lenh go trong cua so controller: arm | disarm | takeoff [do_cao_amsl] | land | help | quit
#include "mavlink_gateway/processing.hpp"
#include "command_manager.hpp"
#include "client_config.hpp"
#include "client_metrics.hpp"
#include "client_log.hpp"
#include "periodic_sampler.hpp"
#include "mavlink_builder.hpp"
#include "net_socket.hpp"
#include "send_queue.hpp"
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <map>
#include <memory>
#include <mutex>
#include <sstream>
#include <string>
#include <thread>
#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

using namespace mavlink_gateway;
using namespace cclient;

namespace {
#ifdef _WIN32
volatile LONG console_stop = 0;
BOOL WINAPI on_console_control(DWORD type) {
    if (type == CTRL_C_EVENT || type == CTRL_BREAK_EVENT) {
        InterlockedExchange(&console_stop, 1); return TRUE;
    }
    return FALSE;
}
bool stop_requested() { return InterlockedCompareExchange(&console_stop, 0, 0) != 0; }
#else
bool stop_requested() { return false; }
#endif
const auto kOrigin = std::chrono::steady_clock::now();
MonoNs now_ns() {
    return static_cast<MonoNs>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - kOrigin).count());
}


std::string describe(const DecodedMessage& m) {
    char b[256];
    if (auto* h = std::get_if<Heartbeat>(&m))
        std::snprintf(b, sizeof b, "HEARTBEAT type=%u autopilot=%u base_mode=%u status=%u", h->type, h->autopilot,
                      h->base_mode, h->system_status);
    else if (auto* s = std::get_if<SysStatus>(&m))
        std::snprintf(b, sizeof b, "SYS_STATUS pin=%.2f V, %d%%", s->battery_voltage_v.value_or(-1.0),
                      s->battery_remaining_percent.value_or(-1));
    else if (auto* t = std::get_if<Attitude>(&m))
        std::snprintf(b, sizeof b, "ATTITUDE roll=%.3f pitch=%.3f yaw=%.3f (rad)", t->roll_rad, t->pitch_rad, t->yaw_rad);
    else if (auto* p = std::get_if<GlobalPosition>(&m))
        std::snprintf(b, sizeof b, "POSITION lat=%.6f lon=%.6f alt_msl=%.2f m rel=%.2f m", p->latitude_deg,
                      p->longitude_deg, p->altitude_msl_m, p->relative_altitude_m);
    else if (auto* k = std::get_if<CommandAck>(&m))
        std::snprintf(b, sizeof b, "COMMAND_ACK command=%u result=%u progress=%u", k->command, k->result, k->progress);
    else b[0] = 0;
    return b;
}

void print_help() {
    std::printf("Lenh: arm | disarm | takeoff [do_cao_amsl_m] | land | help | quit\n");
}
} // namespace

int run_client(const ClientOptions& a) {
    cnet::WinsockInit winsock;
#ifdef _WIN32
    SetConsoleCtrlHandler(on_console_control, TRUE);
#endif

    ClientMetrics measurement(std::uint64_t(a.shared.warmup_seconds) * 1000000000ULL,
        std::uint64_t(a.shared.measurement_seconds) * 1000000000ULL,
        std::uint64_t(a.shared.drain_ms) * 1000000ULL, a.controller ? 1000000U : 0U, now_ns);
    ClientLog log(a, now_ns);
    ProcessProbe process;
    std::printf("Config: %s\n", a.config_path.string().c_str());
    std::string err;
    cnet::Socket sock = cnet::Socket::tcp_connect(a.host, a.port, &err);
    if (!sock.valid()) {
        std::printf("Khong noi duoc %s:%d (%s)\n", a.host.c_str(), a.port, err.c_str());
        measurement.stopped();
        log.event("connect_failed", err);
        log.metrics(now_ns(), measurement.snapshot(), {}, {}, process.sample(), false);
        log.finish(measurement.snapshot(true), {}, process.logical_cpus(), {}, true);
        return 1;
    }
    measurement.connected();
    const auto peer = sock.peer_name();
    std::printf("Da noi toi %s:%d (vai tro: %s)\n", a.host.c_str(), a.port, a.controller ? "controller" : "observer");
    log.event("client_connected", a.host + ":" + std::to_string(a.port));

    Parser parser(a.shared.tcp_buffer_bytes);
    Decoder decoder;
    Inspector inspector;
    std::atomic<bool> running{true};

    // ---- Phan chi co o controller ----
    const GcsId gcs{a.command.gcs_sysid, a.command.gcs_compid};
    std::unique_ptr<SendQueue> sendq;
    auto on_send_completed = [&](const SendCompletion& result) {
        measurement.completed(result);
        log.tx_completed(result);
    };
    auto send_raw = [&](const Bytes& raw) {
        const auto tag = measurement.attempt(now_ns());
        log.tx_attempt(tag, raw.size());
        if (sendq->push(raw, false, tag)) return true;
        on_send_completed({tag, now_ns(), raw.size(), sendq->failed() ?
            DeliveryStatus::ConnectionClosed : DeliveryStatus::ClientQueueFull});
        return false;
    };
    std::unique_ptr<CommandManager> manager;
    std::mutex manager_mutex; // bao ve manager (recv thread, ticker, console cung goi)
    std::thread ticker;
    if (a.controller) {
        sendq = std::make_unique<SendQueue>(sock, a.shared.client_queue_frames, a.shared.client_queue_bytes,
                                          0, 0, now_ns, on_send_completed);
        const CommandConfig cfg = a.command;
        manager = std::make_unique<CommandManager>(
            cfg,
            // The callback outlives this block, so it must own its configuration.
            [gcs, cfg](std::uint16_t cmd, const Params& p, std::uint8_t conf) {
                return build_command_long(gcs, cfg.px4_sysid, cfg.px4_compid, cmd, p, conf);
            },
            [&](const Bytes& raw) { return send_raw(raw); },
            [&](const CommandEvent& e) {
                log.command(e);
                std::printf("[LENH #%llu cmd=%u] %s (lan gui %u)%s\n", (unsigned long long)e.request_id, e.command,
                            e.event.c_str(), e.attempt,
                            e.result ? (" result=" + std::to_string(*e.result)).c_str() : "");
            });

        ticker = std::thread([&] {
            auto next_hb = std::chrono::steady_clock::now();
            const auto heartbeat_period = std::chrono::milliseconds(a.heartbeat_interval_ms);
            const auto tick_ms = std::min<MonoNs>({50, a.heartbeat_interval_ms,
                a.command.ack_timeout_ns / 1000000ULL, a.command.progress_idle_ns / 1000000ULL});
            while (running) {
                {
                    std::lock_guard<std::mutex> lock(manager_mutex);
                    manager->tick(now_ns());
                }
                if (std::chrono::steady_clock::now() >= next_hb) {
                    send_raw(build_gcs_heartbeat(gcs));
                    next_hb = std::chrono::steady_clock::now() + heartbeat_period;
                }
                std::this_thread::sleep_until(std::min(next_hb,
                    std::chrono::steady_clock::now() + std::chrono::milliseconds(tick_ms)));
            }
        });

        std::thread([&] {
            print_help();
            std::string line;
            while (running && std::getline(std::cin, line)) {
                std::istringstream in(line);
                std::string word;
                in >> word;
                if (word.empty()) continue;
                if (word == "quit" || word == "exit") {
                    running = false;
                    sock.shutdown_both();
                    break;
                }
                Params p{};
                std::uint16_t cmd = 0;
                if (word == "arm") { cmd = kCmdArm; p[0] = 1; }
                else if (word == "disarm") { cmd = kCmdArm; p[0] = 0; }
                else if (word == "takeoff") {
                    cmd = kCmdTakeoff;
                    double alt;
                    // CHUA KIEM CHUNG tren PX4 SITL: tham so TAKEOFF can thu that (xem muc 7 giao uoc).
                    if (in >> alt) p[6] = static_cast<float>(alt);
                } else if (word == "land") cmd = kCmdLand;
                else { print_help(); continue; }
                if (std::find(a.shared.allowed_commands.begin(), a.shared.allowed_commands.end(), cmd) == a.shared.allowed_commands.end()) {
                    std::printf("Command %u is disabled by config\n", cmd);
                    log.event("command_not_allowed", std::to_string(cmd));
                    continue;
                }
                std::lock_guard<std::mutex> lock(manager_mutex);
                if (manager->submit(cmd, p, now_ns()) == 0) std::printf("Khong gui duoc lenh (dang cho ACK/da mat ket noi)\n");
            }
        }).detach();
    }

    auto sample_metrics = [&](const SendQueueSnapshot* final_tx = nullptr) {
        const auto tx = final_tx ? *final_tx : sendq ? sendq->snapshot() : SendQueueSnapshot{};
        log.metrics(now_ns(), measurement.snapshot(),
            a.mode == MeasurementMode::B0 ? InspectorSnapshot{} : inspector.snapshot(), tx, process.sample(), running);
    };
    PeriodicSampler sampler(std::chrono::milliseconds(a.shared.snapshot_interval_ms), [&] { sample_metrics(); });

    // ---- Vong nhan (thread chinh) ----
    std::map<std::uint32_t, MonoNs> last_print;
    FrameId next_frame_id = 1;
    std::uint8_t buf[4096];
    while (running && !stop_requested()) {
        if (sendq && sendq->failed()) break;
        if (!sock.readable(100)) continue;
        const long n = sock.recv_some(buf, sizeof buf);
        if (n <= 0) break;

        ReceiveEvent ev;
        ev.source.source_id = 1;
        ev.source.session_id = 1;
        ev.source.transport = Transport::Tcp;
        ev.source.flow = Flow::TelemetryDownlink;
        ev.source.peer = {a.host, static_cast<std::uint16_t>(a.port)};
        ev.received_at_ns = now_ns();
        ev.bytes.assign(buf, buf + n);

        ParseBatch batch = parser.consume(ev);
        for (const auto& i : batch.issues) {
            if (a.mode != MeasurementMode::B0) inspector.on_issue(i);
            log.issue(i);
            std::printf("LOI PARSE: %s\n", issue_name(i.code));
        }
        for (auto& f : batch.frames) {
            f.frame_id = next_frame_id++;
            measurement.received(f.received_at_ns);
            std::optional<DecodedMessage> d;
            // B0 still handles ACKs, including late ACKs, for the controller's command protocol.
            const bool needs_command_ack = manager && f.header.msgid == 77;
            if (a.mode != MeasurementMode::B0 || needs_command_ack) d = decoder.decode(f);
            if (a.mode != MeasurementMode::B0) inspector.on_frame(f, d);
            log.frame(f);
            if (!d) continue;

            if (auto* ack = std::get_if<CommandAck>(&*d)) {
                if (manager) {
                    std::lock_guard<std::mutex> lock(manager_mutex);
                    manager->on_ack(*ack, f.header.sysid, f.header.compid, f.received_at_ns);
                }
                std::printf("%s\n", describe(*d).c_str());
                continue;
            }
            const MonoNs t = now_ns();
            auto it = last_print.find(f.header.msgid);
            if (a.verbose || it == last_print.end() || t - it->second >= 500000000ULL) {
                last_print[f.header.msgid] = t;
                std::printf("%s\n", describe(*d).c_str());
            }
        }
        if (batch.close_tcp_session) {
            std::printf("Parser dong phien (bo dem TCP vuot gioi han)\n");
            break;
        }
    }

    // ---- Don dep ----
    const auto stopped_at = measurement.stopped();
    running = false;
    sampler.stop();
    std::printf("Mat ket noi / ket thuc.\n");
    log.event("connection_closed");
    if (manager) {
        std::lock_guard<std::mutex> lock(manager_mutex);
        manager->on_disconnect(now_ns());
    }
    if (ticker.joinable()) ticker.join();
    SendQueueSnapshot final_tx;
    bool drained = true;
    std::size_t shutdown_pending = 0;
    if (sendq) {
        drained = sendq->drain_until(kOrigin + std::chrono::nanoseconds(stopped_at) + std::chrono::milliseconds(a.shared.drain_ms));
        shutdown_pending = sendq->snapshot().pending_frames;
        sendq->close();
        final_tx = sendq->snapshot();
    }
    sock.shutdown_both();
    sendq.reset();
    sock.close();
    sample_metrics(&final_tx);

    if (a.mode != MeasurementMode::B0) {
        const auto snap = inspector.snapshot();
        std::printf("\n== Thong ke: %llu khung hop le | bad_crc=%llu | unsupported=%llu | truncated=%llu | noise=%llu byte\n",
                    (unsigned long long)snap.valid_frames, (unsigned long long)snap.bad_crc_candidates,
                    (unsigned long long)snap.unsupported_candidates, (unsigned long long)snap.truncated_datagrams,
                    (unsigned long long)snap.discarded_noise_bytes);
        for (const auto& s : snap.sources)
            std::printf("   nguon sysid=%u compid=%u: nhan=%llu, thieu_uoc_luong=%llu, lap=%llu, mo_ho=%llu\n", s.sysid,
                        s.compid, (unsigned long long)s.received_valid, (unsigned long long)s.estimated_missing,
                        (unsigned long long)s.repeated_seq, (unsigned long long)s.ambiguous_seq);
    }
    const auto result = log.finish(measurement.snapshot(true), final_tx, process.logical_cpus(), peer,
                                   drained, sampler.failed(), shutdown_pending);
    std::fflush(stdout);
    if (a.controller) std::_Exit(result); // thread console co the con bi chan o getline
    return result;
}

int main(int argc, char** argv) {
    ClientOptions options;
    try { options = load_client_options(argc, argv); }
    catch (const std::exception& error) {
        std::fprintf(stderr, "Config/CLI error: %s\n", error.what());
        return 2;
    }
    if (options.help) { std::puts(client_usage()); return 0; }
    if (options.print_config) { std::cout << options.resolved_config_json << '\n'; return 0; }
    try { return run_client(options); }
    catch (const std::exception& error) {
        std::fprintf(stderr, "ERROR: %s\n", error.what());
        return 1;
    }
}
