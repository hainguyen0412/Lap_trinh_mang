// TCP test server: exact fixture frames and generated ACKs share one send worker.
#include "mavlink_gateway/processing.hpp"
#include "net_socket.hpp"
#include "send_queue.hpp"
#include <common/mavlink.h>
#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <mutex>
#include <queue>
#include <stdexcept>
#include <string>
#include <thread>

using namespace mavlink_gateway;
using Clock = std::chrono::steady_clock;

namespace {
struct PendingAck {
    Clock::time_point due;
    std::uint64_t order;
    std::uint16_t command;
    std::uint8_t target_system, target_component, result, progress;
};
struct LaterAck {
    bool operator()(const PendingAck& a, const PendingAck& b) const {
        return a.due == b.due ? a.order > b.order : a.due > b.due;
    }
};

class FakeSession {
public:
    FakeSession(cnet::Socket& client, const std::vector<Bytes>& frames,
                std::string mode, std::size_t chunk, bool loop)
        : client_(client), frames_(frames), mode_(std::move(mode)), loop_(loop),
          sender_(client, 1024, 1048576, chunk, 5) {}

    ~FakeSession() {
        stop();
        if (telemetry_.joinable()) telemetry_.join();
        if (acks_.joinable()) acks_.join();
    }

    void run() {
        telemetry_ = std::thread([this] { guard([this] { send_telemetry(); }); });
        acks_ = std::thread([this] { guard([this] { send_acks(); }); });
        Parser parser;
        std::uint8_t buffer[2048];
        while (!done_) {
            if (sender_.failed()) break;
            if (!client_.readable(100)) continue;
            const auto n = client_.recv_some(buffer, sizeof buffer);
            if (n <= 0) break;
            ReceiveEvent event;
            event.source = {1, 1, Transport::Tcp, Flow::ControlUplink, {"client", 0}};
            event.bytes.assign(buffer, buffer + n);
            const auto batch = parser.consume(event);
            if (batch.close_tcp_session) break;
            for (const auto& frame : batch.frames) handle(frame);
        }
        stop();
    }

private:
    void stop() {
        {
            std::lock_guard<std::mutex> lock(timer_mutex_);
            done_ = true;
            while (!pending_.empty()) pending_.pop();
        }
        timer_wake_.notify_all();
        sender_.stop();
    }

    template<class Action> void guard(Action action) {
        try { action(); }
        catch (const std::exception& error) {
            std::fprintf(stderr, "Session worker: %s\n", error.what());
            stop();
        }
    }

    void send_telemetry() {
        do {
            for (const auto& frame : frames_) {
                if (done_) return;
                if (!sender_.push(frame)) {
                    std::fprintf(stderr, "Telemetry send queue full or stopped\n");
                    stop();
                    return;
                }
            }
            if (!loop_) return;
            std::unique_lock<std::mutex> lock(timer_mutex_);
            timer_wake_.wait_for(lock, std::chrono::seconds(1), [this] { return done_.load(); });
        } while (!done_);
    }

    bool schedule(const FrameRecord& frame, std::uint16_t command, std::uint8_t result,
                  std::uint8_t progress, unsigned delay_ms) {
        {
            std::lock_guard<std::mutex> lock(timer_mutex_);
            if (done_ || pending_.size() >= 1024) return false;
            pending_.push({Clock::now() + std::chrono::milliseconds(delay_ms), next_order_++, command,
                           frame.header.sysid, frame.header.compid, result, progress});
        }
        timer_wake_.notify_all();
        return true;
    }

    void send_acks() {
        std::unique_lock<std::mutex> lock(timer_mutex_);
        while (!done_) {
            if (pending_.empty()) {
                timer_wake_.wait(lock, [this] { return done_ || !pending_.empty(); });
                continue;
            }
            const auto job = pending_.top();
            if (Clock::now() < job.due) {
                timer_wake_.wait_until(lock, job.due);
                continue;
            }
            pending_.pop();
            lock.unlock();
            mavlink_message_t message{};
            mavlink_msg_command_ack_pack(1, 1, &message, job.command, job.result, job.progress,
                                        0, job.target_system, job.target_component);
            std::uint8_t raw[MAVLINK_MAX_PACKET_LEN];
            const auto size = mavlink_msg_to_send_buffer(raw, &message);
            if (!sender_.push(Bytes(raw, raw + size), true)) {
                std::fprintf(stderr, "ACK send queue full or stopped\n");
                stop();
                return;
            }
            std::printf("  -> queued COMMAND_ACK command=%u result=%u progress=%u\n",
                        job.command, job.result, job.progress);
            lock.lock();
        }
    }

    void handle(const FrameRecord& frame) {
        if (frame.header.msgid == 0) return;
        if (frame.header.msgid != 76) return;
        std::uint8_t payload[33]{};
        if (frame.header.payload_len > sizeof payload) {
            std::fprintf(stderr, "Ignored oversized COMMAND_LONG\n");
            return;
        }
        std::copy_n(frame.raw.begin() + 10, frame.header.payload_len, payload);
        const auto command = static_cast<std::uint16_t>(payload[28] | (payload[29] << 8));
        std::printf("  <- COMMAND_LONG command=%u target=%u/%u confirmation=%u (tu %u/%u)\n",
                    command, payload[30], payload[31], payload[32], frame.header.sysid, frame.header.compid);
        std::fflush(stdout);
        bool queued = true;
        if (mode_ == "accept") queued = schedule(frame, command, 0, 100, 0);
        else if (mode_ == "reject") queued = schedule(frame, command, 2, 0, 0);
        else if (mode_ == "progress")
            queued = schedule(frame, command, 5, 50, 0) && schedule(frame, command, 0, 100, 1500);
        else if (mode_ == "late") queued = schedule(frame, command, 0, 100, 4000);
        if (!queued) {
            std::fprintf(stderr, "ACK timer queue full or stopped\n");
            stop();
        }
    }

    cnet::Socket& client_;
    const std::vector<Bytes>& frames_;
    const std::string mode_;
    const bool loop_;
    cclient::SendQueue sender_;
    std::atomic<bool> done_{false};
    std::mutex timer_mutex_;
    std::condition_variable timer_wake_;
    std::priority_queue<PendingAck, std::vector<PendingAck>, LaterAck> pending_;
    std::uint64_t next_order_ = 0;
    std::thread telemetry_, acks_;
};

int integer(const std::string& text, int minimum, int maximum) {
    std::size_t used = 0;
    const auto value = std::stoll(text, &used);
    if (used != text.size() || value < minimum || value > maximum)
        throw std::invalid_argument("Integer out of range: " + text);
    return static_cast<int>(value);
}
} // namespace

int main(int argc, char** argv) {
    if (argc < 2 || std::string(argv[1]) == "--help") {
        std::printf("fake_gateway <file.bin> [--port N] [--chunk N] [--loop] [--once] [--ack accept|reject|progress|none|late]\n");
        return argc < 2 ? 2 : 0;
    }
    try {
        const std::string file = argv[1];
        std::string mode = "accept";
        int port = 5760;
        std::size_t chunk = 7;
        bool loop = false, once = false;
        for (int i = 2; i < argc; ++i) {
            const std::string key = argv[i];
            if (key == "--loop") loop = true;
            else if (key == "--once") once = true;
            else if ((key == "--port" || key == "--chunk" || key == "--ack") && i + 1 < argc) {
                const std::string value = argv[++i];
                if (key == "--port") port = integer(value, 1, 65535);
                else if (key == "--chunk") chunk = static_cast<std::size_t>(integer(value, 1, 65536));
                else mode = value;
            } else throw std::invalid_argument("Unknown or incomplete option: " + key);
        }
        if (mode != "accept" && mode != "reject" && mode != "progress" && mode != "none" && mode != "late")
            throw std::invalid_argument("Unknown ACK mode: " + mode);
        std::ifstream input(file, std::ios::binary);
        if (!input) throw std::runtime_error("Khong mo duoc " + file);
        if (std::filesystem::file_size(file) > 16 * 1024 * 1024)
            throw std::invalid_argument("Fixture limit: 16 MiB");
        ReceiveEvent fixture;
        fixture.source = {1, 0, Transport::Udp, Flow::TelemetryDownlink, {"fixture", 0}};
        fixture.bytes = Bytes(std::istreambuf_iterator<char>(input), {});
        Parser parser;
        auto parsed = parser.consume(fixture);
        if (parsed.frames.empty() || !parsed.issues.empty())
            throw std::invalid_argument("Fixture must contain complete valid unsigned MAVLink 2 frames only");
        std::vector<Bytes> frames;
        for (auto& frame : parsed.frames) frames.push_back(std::move(frame.raw));

        cnet::WinsockInit winsock;
        std::string error;
        auto listener = cnet::Socket::tcp_listen(port, &error);
        if (!listener.valid()) throw std::runtime_error(error);
        std::printf("fake_gateway: doi client o cong %d (frames=%zu, ack=%s)\n", port, frames.size(), mode.c_str());
        std::fflush(stdout);
        do {
            std::string peer;
            auto client = listener.accept_client(&peer, &error);
            if (!client.valid()) throw std::runtime_error(error);
            std::printf("Client noi: %s\n", peer.c_str());
            std::fflush(stdout);
            { FakeSession session(client, frames, mode, chunk, loop); session.run(); }
            std::printf("Client dong ket noi.\n");
            std::fflush(stdout);
        } while (!once);
        return 0;
    } catch (const std::exception& error) {
        std::fprintf(stderr, "ERROR: %s\n", error.what());
        return 1;
    }
}
