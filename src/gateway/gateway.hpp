#pragma once
#include "network.hpp"
#include "audit.hpp"
#include "bounded_queue.hpp"
#include <chrono>
#include <mutex>
#include <unordered_map>
#include <array>
#include <thread>

namespace mavlink_gateway {
class Gateway {
public:
    using ParserFactory = std::function<std::unique_ptr<IParser>()>;
private:
    using Clock = std::chrono::steady_clock;
    enum class CloseReason { None, Connection, QueueFull, SendError, Shutdown, Observer, ParserLimit };
    struct Session {
        SourceRef source;
        ClientRole role;
        SocketHandle socket;
        BoundedQueue<SharedFrame> outbound;
        std::atomic<CloseReason> reason{CloseReason::None};
        std::atomic<bool> rx_done{false}, tx_done{false};
        std::mutex route_mutex, stats_mutex;
        std::uint64_t attempted_window = 0, sent_window = 0, bytes_window = 0, latency_seen = 0;
        std::vector<MonoNs> latencies;
        std::thread receiver, sender;
        Session(SourceRef s, ClientRole r, SocketHandle sock, const Config& c)
            : source(std::move(s)), role(r), socket(std::move(sock)), outbound(c.client_queue_frames, c.client_queue_bytes) {}
    };
    Config c_;
    ParserFactory parser_factory_;
    std::shared_ptr<IDecoder> decoder_;
    std::shared_ptr<IInspector> inspector_;
    std::function<void(SourceId)> release_source_;
    std::mutex inspector_lifetime_mutex_;
    std::array<std::uint64_t,3> retired_seq_{};
    std::atomic<std::uint64_t> decoded_frames_{0};
    WinsockRuntime winsock_;
    Audit audit_;
    Clock::time_point epoch_ = Clock::now();
    std::atomic<bool> stop_input_{false}, finish_metrics_{false}, failed_{false};
    bool started_ = false, stopped_ = false;
    SocketHandle udp_, observer_, controller_;
    BoundedQueue<ReceiveEvent> incoming_;
    BoundedQueue<SharedFrame> uplink_;
    std::thread udp_rx_, telemetry_, observer_accept_, controller_accept_, udp_tx_, metrics_;
    std::mutex sessions_mutex_, px4_mutex_, latency_mutex_;
    std::vector<std::shared_ptr<Session>> sessions_;
    std::optional<Endpoint> px4_;
    MonoNs heartbeat_ns_ = 0;
    std::unordered_map<std::string, std::uint64_t> candidates_; // UDP receiver owns this map
    std::atomic<std::uint64_t> next_source_{1}, next_session_{1}, next_frame_{1};
    std::atomic<std::uint64_t> valid_{0}, crc_{0}, unsupported_{0}, truncated_{0}, noise_{0};
    std::atomic<std::uint64_t> udp_datagrams_{0}, udp_drop_{0}, client_drop_{0}, tx_bytes_{0};
    std::atomic<std::uint64_t> window_valid_{0}, eligible_{0}, sent_{0}, max_clients_{0};
    std::atomic<std::uint64_t> completed_{0}, all_eligible_{0}, pending_after_drain_{0};
    std::vector<MonoNs> latencies_;
    std::uint64_t latency_seen_ = 0;
    void guard(const std::string&, const std::function<void()>&);
    void event(const std::string&, const std::string& = "", const SourceRef& = {});
    void issues(const ParseBatch&);
    void frame(FrameRecord&);
    void delivery(const SharedFrame&, Session&, std::size_t, const std::string&, MonoNs = 0);
    void session_summary(Session&);
    bool in_window(MonoNs) const;
    void receive_udp();
    void process_telemetry();
    void accept_clients(ClientRole);
    void receive_client(const std::shared_ptr<Session>&);
    void send_client(const std::shared_ptr<Session>&);
    void send_udp();
    void collect_metrics();
    void close_session(const std::shared_ptr<Session>&, CloseReason);
    void reap();
    bool px4_ready(Endpoint&, std::string&);
    void write_final();
    void retire_source(SourceId);
    std::array<std::uint64_t,3> sequence_totals();
public:
    Gateway(Config, ParserFactory, std::shared_ptr<IDecoder> = {}, std::shared_ptr<IInspector> = {},
            std::function<void(SourceId)> release_source = {});
    ~Gateway();
    void start();
    void stop();
    bool failed() const { return failed_; }
    MonoNs now_ns() const;
};
} // namespace mavlink_gateway
