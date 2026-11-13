#include "gateway.hpp"
#include <nlohmann/json.hpp>
#include <windows.h>
#include <psapi.h>
#include <algorithm>
#include <cmath>
#include <ctime>
#include <iomanip>
#include <iostream>
#include <sstream>
#include <fstream>

namespace mavlink_gateway {
namespace {
template<class T> std::string str(T v) { return std::to_string(v); }
std::string key(const Endpoint& e) { return e.ip + ":" + str(e.port); }
std::uint8_t byte(const FrameRecord& f, std::size_t at) { return at < f.header.payload_len && 10+at < f.raw.size() ? f.raw[10+at] : 0; }
std::string utc_now() {
    const auto t = std::time(nullptr); std::tm tm{}; gmtime_s(&tm, &t);
    std::ostringstream s; s << std::put_time(&tm, "%Y-%m-%dT%H:%M:%SZ"); return s.str();
}
std::uint64_t filetime(FILETIME t) { return (std::uint64_t(t.dwHighDateTime) << 32) | t.dwLowDateTime; }
}
Gateway::Gateway(Config c, ParserFactory factory, std::shared_ptr<IDecoder> d, std::shared_ptr<IInspector> i, std::function<void(SourceId)> release_source)
    : c_(std::move(c)), parser_factory_(std::move(factory)), decoder_(std::move(d)), inspector_(std::move(i)), release_source_(std::move(release_source)),
      audit_(c_), incoming_(c_.udp_queue_datagrams, c_.udp_queue_datagrams), uplink_(c_.udp_uplink_frames, c_.udp_uplink_frames) {
    if (!parser_factory_) throw std::runtime_error("Parser factory required");
    if (c_.mode != "B0" && (!decoder_ || !inspector_))
        throw std::runtime_error("B1/B2 require B decoder and Inspector");
}
Gateway::~Gateway() { try { stop(); } catch (...) {} }
MonoNs Gateway::now_ns() const {
    return std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now() - epoch_).count();
}
void Gateway::event(const std::string& name, const std::string& detail, const SourceRef& source) {
    audit_.log("events", {str(now_ns()), str(source.session_id), str(source.source_id), name, detail});
}
void Gateway::guard(const std::string& name, const std::function<void()>& fn) {
    try { fn(); }
    catch (const std::exception& e) { event("worker_error", name + ": " + e.what()); failed_ = true; stop_input_ = true; }
    catch (...) { event("worker_error", name + ": unknown exception"); failed_ = true; stop_input_ = true; }
}
bool Gateway::in_window(MonoNs ns) const {
    const MonoNs start = MonoNs(c_.warmup_seconds) * 1000000000ULL;
    return ns >= start && (!c_.measurement_seconds || ns < start + MonoNs(c_.measurement_seconds) * 1000000000ULL);
}
void Gateway::issues(const ParseBatch& b) {
    for (const auto& i : b.issues) {
        switch (i.code) {
        case ParseIssueCode::BadCrc: ++crc_; break;
        case ParseIssueCode::UnsupportedMessage: case ParseIssueCode::UnsupportedSigning:
        case ParseIssueCode::UnsupportedIncompatFlags: ++unsupported_; break;
        case ParseIssueCode::TruncatedDatagram: ++truncated_; break;
        case ParseIssueCode::NoiseDiscarded: noise_ += i.discarded_bytes; break;
        default: break;
        }
        if (c_.mode != "B0") inspector_->on_issue(i);
        audit_.log("issues", {str(i.source.source_id), str(i.source.session_id), str(i.received_at_ns), issue_name(i.code),
                   str(i.discarded_bytes), i.msgid ? str(*i.msgid) : ""}, true);
    }
}
void Gateway::frame(FrameRecord& f) {
    f.frame_id = next_frame_++;
    ++valid_;
    if (c_.mode != "B0") {
        auto decoded=decoder_->decode(f);
        if(decoded)++decoded_frames_;
        inspector_->on_frame(f,decoded);
    }
    audit_.log("frames", {str(f.frame_id), str(f.source.source_id), str(f.source.session_id), str(f.received_at_ns),
               str(f.header.sysid), str(f.header.compid), str(f.header.msgid), str(f.header.seq), str(f.header.payload_len), str(f.raw.size())}, true);
}
void Gateway::delivery(const SharedFrame& f, Session& s, std::size_t bytes, const std::string& status, MonoNs done) {
    ++completed_;
    if (in_window(f->received_at_ns)) {
        std::lock_guard<std::mutex> lock(s.stats_mutex);
        ++s.attempted_window; s.bytes_window += bytes;
        if (done) {
            ++s.sent_window; ++s.latency_seen;
            if (s.latencies.size() < 100000) s.latencies.push_back(done - f->received_at_ns);
        }
    }
    if (done && in_window(f->received_at_ns)) {
        ++sent_;
        std::lock_guard<std::mutex> lock(latency_mutex_);
        ++latency_seen_;
        if (latencies_.size() < 1000000) latencies_.push_back(done - f->received_at_ns);
    }
    audit_.log("delivery", {str(f->frame_id), str(s.source.session_id), str(now_ns()), done ? str(done) : "", str(bytes), status}, true);
}
void Gateway::session_summary(Session& s) {
    // Receiver/sender are joined; route lock excludes a last fan-out attempt from a snapshot.
    std::lock_guard<std::mutex> route(s.route_mutex);
    std::lock_guard<std::mutex> stats(s.stats_mutex);
    std::sort(s.latencies.begin(), s.latencies.end());
    auto p = [&](double q) { return s.latencies.empty() ? std::string() :
        str(s.latencies[static_cast<std::size_t>(std::ceil(q * s.latencies.size())) - 1] / 1e6); };
    audit_.log("client_summary", {str(s.source.session_id), s.role == ClientRole::Observer ? "observer" : "controller",
        str(s.attempted_window), str(s.sent_window), str(s.attempted_window - s.sent_window), str(s.bytes_window),
        str(s.latencies.size()), p(.95), p(1), s.latency_seen > s.latencies.size() ? "1" : "0"});
}
void Gateway::start() {
    if (started_ || stopped_) throw std::runtime_error("Gateway is single-use");
    udp_ = bound_socket({c_.udp_bind_host, c_.udp_port}, false);
    observer_ = bound_socket({c_.observer_bind_host, c_.observer_port}, true);
    controller_ = bound_socket({c_.controller_bind_host, c_.controller_port}, true);
    started_ = true;
    event("gateway_started", "UDP " + str(c_.udp_port) + "; observer " + str(c_.observer_port) + "; controller " + str(c_.controller_port));
    udp_rx_ = std::thread([this] { guard("udp_receive", [this] { receive_udp(); }); });
    telemetry_ = std::thread([this] { guard("telemetry", [this] { process_telemetry(); }); });
    udp_tx_ = std::thread([this] { guard("udp_send", [this] { send_udp(); }); });
    observer_accept_ = std::thread([this] { guard("observer_accept", [this] { accept_clients(ClientRole::Observer); }); });
    controller_accept_ = std::thread([this] { guard("controller_accept", [this] { accept_clients(ClientRole::Controller); }); });
    metrics_ = std::thread([this] { guard("metrics", [this] { collect_metrics(); }); });
    std::cout << "READY udp=" << c_.udp_port << " observer=" << c_.observer_port << " controller=" << c_.controller_port << std::endl;
}
void Gateway::receive_udp() {
    std::uint8_t buffer[65536];
    while (!stop_input_) {
        if (!ready(udp_.get(), false)) continue;
        sockaddr_in from{}; int from_size = sizeof(from);
        const int n = recvfrom(udp_.get(), reinterpret_cast<char*>(buffer), sizeof(buffer), 0, reinterpret_cast<sockaddr*>(&from), &from_size);
        const auto received = now_ns();
        if (n == SOCKET_ERROR) {
            const int e = WSAGetLastError();
            if (e == WSAEWOULDBLOCK || e == WSAECONNRESET) continue;
            if (e == WSAEMSGSIZE) { event("udp_receive_truncated"); continue; }
            throw std::runtime_error("recvfrom: " + str(e));
        }
        const auto peer = endpoint(from);
        ++udp_datagrams_;
        {
            std::lock_guard<std::mutex> lock(px4_mutex_);
            if (px4_ && !same_endpoint(*px4_,peer)) { event("unexpected_source", key(peer)); continue; }
        }
        auto it = candidates_.find(key(peer));
        if (it == candidates_.end()) {
            if (candidates_.size() >= c_.max_udp_candidates) { event("udp_candidate_limit", key(peer)); continue; }
            it = candidates_.emplace(key(peer), next_source_++).first;
            event("udp_candidate", key(peer), {it->second, 0, Transport::Udp, Flow::TelemetryDownlink, peer});
        }
        ReceiveEvent e{{it->second, 0, Transport::Udp, Flow::TelemetryDownlink, peer}, received, Bytes(buffer, buffer + n)};
        if (!incoming_.try_push(std::move(e))) ++udp_drop_;
    }
}
void Gateway::process_telemetry() {
    auto parser = parser_factory_();
    if (!parser) throw std::runtime_error("Null parser");
    ReceiveEvent e;
    std::vector<std::uint64_t> sources;
    while (!incoming_.closed() || incoming_.size()) {
        if (!incoming_.pop(e)) continue;
        {
            std::lock_guard<std::mutex> lock(px4_mutex_);
            if (px4_ && !same_endpoint(*px4_,e.source.peer)) { event("unexpected_source", key(e.source.peer)); continue; }
        }
        if (std::find(sources.begin(), sources.end(), e.source.source_id) == sources.end()) sources.push_back(e.source.source_id);
        auto batch = parser->consume(e); issues(batch);
        bool locked_now = false;
        {
            std::lock_guard<std::mutex> lock(px4_mutex_);
            for (const auto& f : batch.frames) {
                if (f.header.msgid == 0 && f.header.sysid == c_.px4_sysid && f.header.compid == c_.px4_compid) {
                    if (!px4_) { px4_ = e.source.peer; locked_now = true; }
                    heartbeat_ns_ = f.received_at_ns;
                }
            }
            if (!px4_) continue;
        }
        if (locked_now) event("px4_locked", key(e.source.peer), e.source);
        for (auto& f : batch.frames) {
            frame(f);
            if (in_window(f.received_at_ns)) ++window_valid_;
            auto shared = std::make_shared<const FrameRecord>(std::move(f));
            std::vector<std::shared_ptr<Session>> clients;
            { std::lock_guard<std::mutex> lock(sessions_mutex_); clients = sessions_; }
            for (const auto& s : clients) {
                std::lock_guard<std::mutex> route(s->route_mutex);
                if (s->reason != CloseReason::None) continue;
                ++all_eligible_;
                if (in_window(shared->received_at_ns)) ++eligible_;
                if (!s->outbound.try_push(shared, shared->raw.size())) {
                    if (s->reason == CloseReason::None) {
                        ++client_drop_; delivery(shared, *s, 0, "client_queue_full");
                        close_session(s, CloseReason::QueueFull);
                    } else delivery(shared, *s, 0, "connection_closed");
                }
            }
        }
    }
    for (auto id : sources) { parser->reset(id); retire_source(id); } // same worker as consume()
}
void Gateway::accept_clients(ClientRole role) {
    const auto listener = role == ClientRole::Observer ? observer_.get() : controller_.get();
    while (!stop_input_) {
        reap();
        if (!ready(listener, false)) continue;
        sockaddr_in peer{}; int size = sizeof(peer);
        SocketHandle socket(accept(listener, reinterpret_cast<sockaddr*>(&peer), &size));
        if (socket.get() == INVALID_SOCKET) {
            const int e = WSAGetLastError(); if (e == WSAEWOULDBLOCK) continue;
            throw std::runtime_error("accept: " + str(e));
        }
        u_long nonblocking = 1;
        if (ioctlsocket(socket.get(), FIONBIO, &nonblocking)) throw std::runtime_error("accepted FIONBIO failed");
        const BOOL yes = TRUE;
        setsockopt(socket.get(), IPPROTO_TCP, TCP_NODELAY, reinterpret_cast<const char*>(&yes), sizeof(yes));
        if (c_.socket_send_buffer_bytes)
            setsockopt(socket.get(), SOL_SOCKET, SO_SNDBUF, reinterpret_cast<const char*>(&c_.socket_send_buffer_bytes), sizeof(int));
        std::lock_guard<std::mutex> lock(sessions_mutex_);
        const auto count = std::count_if(sessions_.begin(), sessions_.end(), [role](const auto& s) { return s->role == role; });
        if (count >= static_cast<long long>(role == ClientRole::Observer ? c_.max_observers : 1)) {
            event(role == ClientRole::Observer ? "observer_limit" : "controller_busy", key(endpoint(peer))); continue;
        }
        SourceRef source{next_source_++, next_session_++, Transport::Tcp, Flow::ControlUplink, endpoint(peer)};
        auto s = std::make_shared<Session>(source, role, std::move(socket), c_);
        sessions_.push_back(s);
        max_clients_ = std::max<std::uint64_t>(max_clients_, sessions_.size());
        event("client_connected", role == ClientRole::Observer ? "observer" : "controller", source);
        s->receiver = std::thread([this, s] { guard("client_receive", [this, s] { receive_client(s); });
            if (!stop_input_ || failed_) close_session(s, CloseReason::Connection);
            s->rx_done = true; });
        s->sender = std::thread([this, s] { guard("client_send", [this, s] { send_client(s); });
            close_session(s, CloseReason::SendError); s->tx_done = true; });
    }
}
void Gateway::close_session(const std::shared_ptr<Session>& s, CloseReason reason) {
    auto expected = CloseReason::None;
    if (!s->reason.compare_exchange_strong(expected, reason)) return;
    s->outbound.close();
    shutdown(s->socket.get(), SD_BOTH); // Handle itself remains valid until both workers join.
    std::string why;
    switch (reason) {
    case CloseReason::QueueFull: why = "client_queue_full"; break;
    case CloseReason::Observer: why = "observer_uplink_denied"; break;
    case CloseReason::ParserLimit: why = "tcp_buffer_limit"; break;
    case CloseReason::Shutdown: why = "shutdown_discard"; break;
    case CloseReason::SendError: why = "send_error"; break;
    default: why = "connection_closed"; break;
    }
    event(why, "session closing", s->source);
}
void Gateway::reap() {
    std::vector<std::shared_ptr<Session>> done;
    {
        std::lock_guard<std::mutex> lock(sessions_mutex_);
        auto it = sessions_.begin();
        while (it != sessions_.end()) {
            if ((*it)->rx_done && (*it)->tx_done) { done.push_back(*it); it = sessions_.erase(it); }
            else ++it;
        }
    }
    for (auto& s : done) {
        if (s->receiver.joinable()) s->receiver.join();
        if (s->sender.joinable()) s->sender.join();
        session_summary(*s);
        s->socket.reset();
        event("client_reaped", "socket closed once", s->source);
    }
}
bool Gateway::px4_ready(Endpoint& endpoint_out, std::string& error) {
    std::lock_guard<std::mutex> lock(px4_mutex_);
    if (!px4_) { error = "px4_endpoint_unknown"; return false; }
    if (c_.reject_commands_when_stale && now_ns() - heartbeat_ns_ > MonoNs(c_.heartbeat_stale_ms) * 1000000) { error = "heartbeat_stale"; return false; }
    endpoint_out = *px4_; return true;
}
void Gateway::receive_client(const std::shared_ptr<Session>& s) {
    auto parser = parser_factory_(); if (!parser) throw std::runtime_error("Null parser");
    std::uint8_t buffer[8192];
    try {
        while (!stop_input_ && s->reason == CloseReason::None) {
            if (!ready(s->socket.get(), false)) continue;
            int n = recv(s->socket.get(), reinterpret_cast<char*>(buffer), sizeof(buffer), 0);
            const auto received = now_ns();
            if (n == 0) break;
            if (n == SOCKET_ERROR) {
                auto error = WSAGetLastError(); if (error == WSAEWOULDBLOCK) continue;
                if (s->reason == CloseReason::None) event("recv_error", str(error), s->source);
                break;
            }
            if (s->role == ClientRole::Observer) { close_session(s, CloseReason::Observer); break; }
            auto batch = parser->consume({s->source, received, Bytes(buffer, buffer + n)}); issues(batch);
            for (auto& f : batch.frames) {
                frame(f);
                Endpoint px4; std::string error;
                if (!px4_ready(px4, error)) { event(error, "uplink rejected", s->source); continue; }
                if (f.header.sysid != c_.gcs_sysid || f.header.compid != c_.gcs_compid) { event("uplink_not_allowed", "GCS ID", s->source); continue; }
                if (f.header.msgid == 76) {
                    const auto command = byte(f, 28) | (byte(f, 29) << 8);
                    if (std::find(c_.allowed_commands.begin(),c_.allowed_commands.end(),command)==c_.allowed_commands.end()) { event("uplink_not_allowed", "command=" + str(command), s->source); continue; }
                    if (byte(f, 30) != c_.px4_sysid || byte(f, 31) != c_.px4_compid) { event("target_mismatch", "COMMAND_LONG", s->source); continue; }
                } else if (f.header.msgid != 0) { event("uplink_not_allowed", "msgid=" + str(f.header.msgid), s->source); continue; }
                if (!uplink_.try_push(std::make_shared<const FrameRecord>(std::move(f)))) event("udp_uplink_queue_full", "frame dropped", s->source);
            }
            if (batch.close_tcp_session) { close_session(s, CloseReason::ParserLimit); break; }
        }
    } catch (...) { parser->reset(s->source.source_id); retire_source(s->source.source_id); throw; }
    parser->reset(s->source.source_id); retire_source(s->source.source_id); // Never called from reaper/network manager.
}
void Gateway::send_client(const std::shared_ptr<Session>& s) {
    SharedFrame f;
    while (!s->outbound.closed() || s->outbound.size()) {
        if (!s->outbound.pop(f)) continue;
        if (s->reason != CloseReason::None) {
            delivery(f, *s, 0, s->reason == CloseReason::Shutdown ? "shutdown_discard" : "connection_closed"); continue;
        }
        const auto begin = Clock::now();
        SendResult result;
        try {
            result = send_fully(f->raw,
                [&](const std::uint8_t* bytes, int n) { return send(s->socket.get(), reinterpret_cast<const char*>(bytes), n, 0); },
                [&] { return ready(s->socket.get(), true); },
                [&] { return s->reason != CloseReason::None || Clock::now() - begin > std::chrono::milliseconds(c_.send_timeout_ms); },
                c_.send_chunk_bytes);
        } catch (const std::exception& e) { result.error = e.what(); }
        tx_bytes_ += result.bytes;
        if (result.complete) delivery(f, *s, result.bytes, "sent_to_socket", now_ns());
        else {
            const auto status = s->reason == CloseReason::Shutdown ? "shutdown_discard" :
                s->reason != CloseReason::None ? "connection_closed" : "send_error";
            delivery(f, *s, result.bytes, status);
            event("send_incomplete", result.error + "; bytes=" + str(result.bytes), s->source);
            close_session(s, CloseReason::SendError);
        }
    }
}
void Gateway::send_udp() {
    SharedFrame f;
    while (!uplink_.closed() || uplink_.size()) {
        if (!uplink_.pop(f)) continue;
        Endpoint peer; std::string error;
        if (!px4_ready(peer, error)) { event(error, "queued uplink dropped", f->source); continue; }
        auto addr = address(peer);
        const int n = sendto(udp_.get(), reinterpret_cast<const char*>(f->raw.data()), static_cast<int>(f->raw.size()), 0,
                             reinterpret_cast<sockaddr*>(&addr), sizeof(addr));
        if (n != static_cast<int>(f->raw.size())) event("udp_send_error", str(WSAGetLastError()), f->source);
        else event("uplink_sent", "msgid=" + str(f->header.msgid) + "; endpoint=" + key(peer), f->source);
    }
}
void Gateway::retire_source(SourceId id) {
    if(!release_source_||c_.mode=="B0")return;
    std::lock_guard<std::mutex> lock(inspector_lifetime_mutex_);
    for(const auto& source:inspector_->snapshot().sources)if(source.source_id==id){
        retired_seq_[0]+=source.estimated_missing;retired_seq_[1]+=source.repeated_seq;retired_seq_[2]+=source.ambiguous_seq;
    }
    release_source_(id);
}
std::array<std::uint64_t,3> Gateway::sequence_totals(){
    std::lock_guard<std::mutex> lock(inspector_lifetime_mutex_);
    auto totals=retired_seq_;
    if(c_.mode!="B0"&&inspector_)for(const auto& source:inspector_->snapshot().sources){
        totals[0]+=source.estimated_missing;totals[1]+=source.repeated_seq;totals[2]+=source.ambiguous_seq;
    }
    return totals;
}
void Gateway::collect_metrics() {
    auto last = Clock::now(); std::uint64_t last_valid = 0, last_tx = 0, last_cpu = 0;
    SYSTEM_INFO si{}; GetSystemInfo(&si);
    FILETIME created{}, exited{}, kernel{}, user{};
    GetProcessTimes(GetCurrentProcess(), &created, &exited, &kernel, &user); last_cpu = filetime(kernel) + filetime(user);
    while (!finish_metrics_) {
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
        const auto current = Clock::now();
        if (current - last < std::chrono::milliseconds(c_.snapshot_interval_ms)) continue;
        const double seconds = std::chrono::duration<double>(current - last).count();
        auto v = valid_.load(), tx = tx_bytes_.load();
        std::size_t clients = 0, max_queue = 0;
        {
            std::lock_guard<std::mutex> lock(sessions_mutex_);
            for (const auto& s : sessions_) if (s->reason == CloseReason::None) { ++clients; max_queue = std::max(max_queue, s->outbound.size()); }
        }
        PROCESS_MEMORY_COUNTERS mem{}; mem.cb = sizeof(mem); GetProcessMemoryInfo(GetCurrentProcess(), &mem, sizeof(mem));
        GetProcessTimes(GetCurrentProcess(), &created, &exited, &kernel, &user);
        const auto cpu = filetime(kernel) + filetime(user);
        const double percent = (cpu - last_cpu) * 1e-7 / seconds / si.dwNumberOfProcessors * 100;
        const auto seq=c_.mode=="B0"?std::array<std::uint64_t,3>{}:sequence_totals();
        audit_.log("metrics", {str(now_ns()), str(v), str(crc_.load()), str(unsupported_.load()), str(truncated_.load()), str(noise_.load()),
            c_.mode == "B0" ? "" : str(seq[0]), c_.mode == "B0" ? "" : str(seq[1]),
            c_.mode == "B0" ? "" : str(seq[2]), str(udp_drop_.load()), str(client_drop_.load()), str(audit_.dropped()),
            str(clients), str((v - last_valid) / seconds), str((tx - last_tx) / seconds), str(percent), str(mem.WorkingSetSize), str(incoming_.size()), str(max_queue)});
        last = current; last_valid = v; last_tx = tx; last_cpu = cpu;
    }
}
void Gateway::stop() {
    if (stopped_) return;
    stopped_ = true; stop_input_ = true;
    auto join = [](std::thread& t) { if (t.joinable()) t.join(); };
    join(observer_accept_); join(controller_accept_); join(udp_rx_);
    incoming_.close(); join(telemetry_);
    std::vector<std::shared_ptr<Session>> inputs;
    { std::lock_guard<std::mutex> lock(sessions_mutex_); inputs = sessions_; }
    for (auto& s : inputs) join(s->receiver);
    uplink_.close(); join(udp_tx_);
    // Client receiver exits on stop_input_. Do not close outbound yet: keep sender alive for drain.
    const auto deadline = Clock::now() + std::chrono::milliseconds(c_.drain_ms);
    while (completed_ < all_eligible_ && Clock::now() < deadline && !failed_)
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    pending_after_drain_ = all_eligible_.load() - completed_.load();
    std::vector<std::shared_ptr<Session>> remaining;
    { std::lock_guard<std::mutex> lock(sessions_mutex_); remaining = sessions_; sessions_.clear(); }
    for (auto& s : remaining) close_session(s, CloseReason::Shutdown);
    for (auto& s : remaining) { join(s->receiver); join(s->sender); session_summary(*s); s->socket.reset(); }
    udp_.reset(); observer_.reset(); controller_.reset();
    finish_metrics_ = true; join(metrics_);
    event("gateway_stopped", "pending_after_drain=" + str(pending_after_drain_.load()));
    audit_.close(); write_final();
}
void Gateway::write_final() {
    nlohmann::json j;
    j["schema_version"] = 1; j["run_id"] = c_.run_id; j["component"] = "gateway";
    j["started_at_iso8601"] = utc_now();
    // Approximate wall-clock start; all measured durations use steady_clock exclusively.
    auto wall_start = std::time(nullptr) - static_cast<std::time_t>(now_ns() / 1000000000ULL);
    std::tm tm{}; gmtime_s(&tm, &wall_start); std::ostringstream stamp; stamp << std::put_time(&tm, "%Y-%m-%dT%H:%M:%SZ");
    j["started_at_iso8601"] = stamp.str();
    j["os"] = "Windows";
#ifdef __VERSION__
    j["compiler"] = __VERSION__;
#else
    j["compiler"] = "MSVC/other C++17 compiler";
#endif
    j["network"] = "IPv4/Winsock; see resolved config and endpoint";
    SYSTEM_INFO sys{}; GetSystemInfo(&sys); j["logical_processors"] = sys.dwNumberOfProcessors;
    MEMORYSTATUSEX memory{}; memory.dwLength = sizeof(memory); GlobalMemoryStatusEx(&memory); j["physical_memory_bytes"] = memory.ullTotalPhys;
    j["program_commit"] = nullptr; j["mavlink_commit"] = "41874cfb2af7e658ca9ed88a13e91c750bcd2263"; j["px4_version"] = nullptr;
    j["parser"] = "B Parser: unsigned MAVLink 2 common dialect, shared project contracts";
    j["clock_policy"] = "integer nanoseconds since gateway steady_clock epoch; no cross-process subtraction";
    j["seed"] = c_.seed; j["log_drop"] = audit_.dropped(); j["log_truncated"] = audit_.truncated();
    j["logging_failed"] = audit_.failed(); j["worker_failed"] = failed_.load();
    j["latency_storage"] = "first 1000000 successful frame-client deliveries in measurement window";
    j["latency_total_successes"] = latency_seen_; j["latency_sampling_truncated"] = latency_seen_ > latencies_.size();
    j["client_latency_storage"] = "first 100000 successful samples per session in measurement window; client_summary.csv";
    j["network_counters"] = {{"udp_datagrams", udp_datagrams_.load()}, {"udp_queue_drop", udp_drop_.load()},
                             {"client_queue_drop", client_drop_.load()}, {"tx_bytes", tx_bytes_.load()}};
    { std::lock_guard<std::mutex> lock(px4_mutex_); j["px4_endpoint"] = px4_ ? nlohmann::json{{"ip", px4_->ip}, {"port", px4_->port}} : nlohmann::json(nullptr); }
    j["resolved_config"] = nlohmann::json::parse(c_.resolved_config_json);
    j["resolved_config"]["logging"]["root_directory"] = c_.output_root;
    j["runtime"] = {{"run_id",c_.run_id},{"stop_file",c_.stop_file},{"run_seconds",c_.run_seconds},
        {"send_timeout_ms",c_.send_timeout_ms},{"socket_send_buffer_bytes",c_.socket_send_buffer_bytes},{"send_chunk_bytes",c_.send_chunk_bytes}};
    const auto seq=sequence_totals();
    j["inspector"]={{"enabled",c_.mode!="B0"},{"decoded_frames",decoded_frames_.load()},
        {"seq_gap_total",seq[0]},{"seq_repeat_total",seq[1]},{"seq_ambiguous_total",seq[2]}};
    std::ofstream meta(audit_.directory / "run.json"); meta << j.dump(2) << '\n';
    meta.flush();
    if (!meta) throw std::runtime_error("Cannot write run.json");
    std::sort(latencies_.begin(), latencies_.end());
    auto percentile = [&](double p) {
        if (latencies_.empty()) return std::string();
        const auto index = static_cast<std::size_t>(std::ceil(p * latencies_.size())) - 1;
        return str(latencies_[index] / 1e6);
    };
    const double elapsed = now_ns() / 1e9;
    const double duration = std::max(0.0, std::min(elapsed - c_.warmup_seconds,
                        c_.measurement_seconds ? double(c_.measurement_seconds) : elapsed));
    std::ofstream summary(audit_.directory / "summary.csv", std::ios::binary);
    summary << "run_id,component,mode,clients,measurement_s,valid_window,sent_window,not_sent_window,pending_after_drain,latency_samples,latency_p50_ms,latency_p95_ms,latency_max_ms,log_drop\n";
    summary << csv_row({c_.run_id, "gateway", c_.mode, str(max_clients_.load()), str(duration), str(window_valid_.load()), str(sent_.load()),
        str(eligible_.load() - sent_.load()), str(pending_after_drain_.load()), str(latencies_.size()), percentile(.5), percentile(.95), percentile(1), str(audit_.dropped())});
    summary.flush();
    if (!summary) throw std::runtime_error("Cannot write summary.csv");
    if (audit_.failed()) throw std::runtime_error("Log write failed; inspect run.json");
}
} // namespace mavlink_gateway
