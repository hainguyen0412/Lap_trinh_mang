#include "network.hpp"
#include "mavlink_gateway/processing.hpp"
#include "bounded_queue.hpp"
#include <future>
#include <iostream>

namespace {
void check(bool ok, const char* message) { if (!ok) throw std::runtime_error(message); }
mavlink_gateway::Bytes hex(const std::string& s) {
    mavlink_gateway::Bytes b; for (std::size_t i = 0; i < s.size(); i += 2) b.push_back(static_cast<std::uint8_t>(std::stoi(s.substr(i, 2), nullptr, 16))); return b;
}
}
int main() {
    try {
        mavlink_gateway::WinsockRuntime runtime;
        mavlink_gateway::BoundedQueue<int> q(2, 3);
        check(q.try_push(1, 2), "queue initial push");
        check(!q.try_push(2, 2), "queue byte limit");
        check(q.try_push(2, 1), "queue exact weight limit");
        check(!q.try_push(3, 0), "queue frame limit");
        int out; check(q.pop(out) && out == 1, "queue FIFO"); check(q.pop(out) && out == 2, "queue FIFO second");
        auto waiting = std::async(std::launch::async, [&] { int item; return q.pop(item, 5000); });
        q.close();
        check(waiting.wait_for(std::chrono::milliseconds(300)) == std::future_status::ready && !waiting.get(), "close wakes waiter");
        check(!q.try_push(4), "no push after close");
        std::cout << "PASS U12: count/bytes limits, FIFO, close/wakeup\n";

        mavlink_gateway::Bytes input{0, 1, 2, 3, 4, 5, 6, 7, 8}, output;
        int calls = 0;
        auto sent = mavlink_gateway::send_fully(input, [&](const std::uint8_t* p, int n) {
            ++calls; if (calls == 2) { WSASetLastError(WSAEWOULDBLOCK); return SOCKET_ERROR; }
            int take = std::min(n, 2); output.insert(output.end(), p, p + take); return take;
        }, [] { return true; }, [] { return false; });
        check(sent.complete && sent.bytes == input.size() && output == input, "partial send exact bytes");
        auto cancelled = mavlink_gateway::send_fully(input, [](auto, int) { return 1; }, [] { return false; }, [] { return true; });
        check(!cancelled.complete && cancelled.bytes == 0, "cancel before blocked send");
        auto zero = mavlink_gateway::send_fully(input, [](auto, int) { return 0; }, [] { return true; }, [] { return false; });
        check(!zero.complete && zero.error == "zero_write", "zero send exits");
        std::cout << "PASS partial send: short write, would-block, cancel, zero progress\n";

        // Fixed HEARTBEAT vector checked against independent Python fixture test.
        const auto raw = hex("fd090000000101000000000000000203510403e71e");
        mavlink_gateway::Parser parser;
        mavlink_gateway::SourceRef a{1, 1, mavlink_gateway::Transport::Tcp, mavlink_gateway::Flow::ControlUplink, {"127.0.0.1", 5000}};
        mavlink_gateway::SourceRef b = a; b.source_id = 2; b.session_id = 2;
        auto first = parser.consume({a, 1, mavlink_gateway::Bytes(raw.begin(), raw.begin() + 8)});
        check(first.frames.empty(), "TCP waits for incomplete header");
        auto other = parser.consume({b, 2, mavlink_gateway::Bytes(raw.begin() + 8, raw.end())});
        check(other.frames.empty(), "sessions do not share buffer");
        auto last = parser.consume({a, 3, mavlink_gateway::Bytes(raw.begin() + 8, raw.end())});
        check(last.frames.size() == 1 && last.frames[0].raw == raw && last.frames[0].received_at_ns == 3, "complete frame exact raw/time");
        parser.reset(1);
        check(parser.consume({a, 4, mavlink_gateway::Bytes(raw.begin() + 8, raw.end())}).frames.empty(), "reset discards old data");
        a.transport = mavlink_gateway::Transport::Udp; a.session_id = 0; a.source_id = 3;
        auto truncated = parser.consume({a, 5, mavlink_gateway::Bytes(raw.begin(), raw.begin() + 8)});
        check(truncated.frames.empty() && !truncated.issues.empty() && truncated.issues.back().code == mavlink_gateway::ParseIssueCode::TruncatedDatagram, "UDP truncated datagram");
        check(parser.consume({a, 6, mavlink_gateway::Bytes(raw.begin() + 8, raw.end())}).frames.empty(), "UDP does not join datagrams");
        std::cout << "PASS adapter boundary checks: TCP isolation/reset, UDP boundary, raw/time\n";
        std::cout << "ALL UNIT TESTS PASSED\n"; return 0;
    } catch (const std::exception& e) { std::cerr << "FAIL: " << e.what() << '\n'; return 1; }
}
