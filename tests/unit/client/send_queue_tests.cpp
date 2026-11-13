#include "net_socket.hpp"
#include "send_queue.hpp"
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <thread>

using Bytes = mavlink_gateway::Bytes;
using Clock = std::chrono::steady_clock;

namespace {
void check(bool value, const char* message) {
    if (!value) throw std::runtime_error(message);
}

template<class Predicate> void wait_for(Predicate predicate) {
    const auto until = Clock::now() + std::chrono::seconds(1);
    while (!predicate()) {
        check(Clock::now() < until, "Worker did not finish within one second");
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
}

struct Pair {
    cnet::Socket client, peer;
    Pair() {
        std::string error;
        auto listener = cnet::Socket::tcp_listen(0, &error);
        check(listener.valid(), "Cannot create loopback listener");
        sockaddr_in address{};
#ifdef _WIN32
        int length = sizeof address;
#else
        socklen_t length = sizeof address;
#endif
        check(getsockname(listener.handle(), reinterpret_cast<sockaddr*>(&address), &length) == 0,
              "Cannot find loopback port");
        client = cnet::Socket::tcp_connect("127.0.0.1", ntohs(address.sin_port), &error);
        check(client.valid(), "Cannot connect loopback pair");
        peer = listener.accept_client(nullptr, &error);
        check(peer.valid(), "Cannot accept loopback pair");
    }
};

Bytes receive(cnet::Socket& socket, std::size_t length) {
    Bytes data(length);
    std::size_t offset = 0;
    while (offset < length) {
        const auto count = socket.recv_some(data.data() + offset, length - offset);
        check(count > 0, "Connection closed before expected data");
        offset += static_cast<std::size_t>(count);
    }
    return data;
}

void priority_between_frames() {
    Pair pair;
    cclient::SendQueue sender(pair.client, 8, 1024, 1, 3);
    check(sender.push(Bytes(24, 'A')), "Cannot enqueue first frame");
    const auto first = receive(pair.peer, 1);
    check(first[0] == 'A', "First frame did not start");
    check(sender.push(Bytes(12, 'B')), "Cannot enqueue telemetry");
    check(sender.push(Bytes(5, 'C'), true), "Cannot enqueue first priority frame");
    check(sender.push(Bytes(6, 'D'), true), "Cannot enqueue second priority frame");
    auto actual = first;
    const auto tail = receive(pair.peer, 46);
    actual.insert(actual.end(), tail.begin(), tail.end());
    Bytes expected(24, 'A');
    for (const auto& part : {Bytes(5, 'C'), Bytes(6, 'D'), Bytes(12, 'B')})
        expected.insert(expected.end(), part.begin(), part.end());
    check(actual == expected, "Priority frame interrupted another frame or reversed ACK order");
    wait_for([&] { return sender.bytes_sent() == expected.size(); });
    sender.stop();
    check(sender.dropped() == 0 && !sender.failed(), "Completed frames counted as dropped/failed");
}

void stop_blocked_send() {
    Pair pair;
    const int small_buffer = 1024;
    setsockopt(pair.client.handle(), SOL_SOCKET, SO_SNDBUF,
               reinterpret_cast<const char*>(&small_buffer), sizeof small_buffer);
    setsockopt(pair.peer.handle(), SOL_SOCKET, SO_RCVBUF,
               reinterpret_cast<const char*>(&small_buffer), sizeof small_buffer);
    // Use many bounded writes. Windows loopback can accept a single huge send
    // into its local buffers despite SO_SNDBUF, so that is not a blocking probe.
    auto sender = std::make_unique<cclient::SendQueue>(pair.client, 8, 16 * 1024 * 1024, 1024);
    check(sender->push(Bytes(8 * 1024 * 1024, 'X')), "Cannot enqueue large frame");
    receive(pair.peer, 1); // The worker is inside send_all; the peer keeps its socket open.
    check(sender->bytes_sent() == 0, "Probe did not block the large send before shutdown");
    check(sender->push(Bytes(10, 'Y')) && sender->push(Bytes(10, 'Z'), true), "Cannot enqueue pending frames");
    const auto started = Clock::now();
    sender->stop();
    // Windows may report the blocked write as accepted when shutdown wakes it.
    // Count completed and cancelled frames together instead of assuming all three are cancelled.
    try { wait_for([&] {
        const auto sent = sender->bytes_sent();
        const auto completed = (sent >= 8 * 1024 * 1024 ? 1U : 0U) + (sent % (8 * 1024 * 1024)) / 10;
        return completed + sender->dropped() == 3;
    }); }
    catch (...) {
        std::printf("Blocked-send snapshot: sent=%llu dropped=%llu failed=%d\n",
                    (unsigned long long)sender->bytes_sent(), (unsigned long long)sender->dropped(),
                    sender->failed() ? 1 : 0);
        throw;
    }
    check(!sender->failed(), "Intentional shutdown counted as a socket error");
    check(!sender->push(Bytes{1}), "Stopped queue accepted another frame");
    sender.reset();
    check(Clock::now() - started < std::chrono::seconds(1), "Destructor hung on blocked send");
}

void stop_chunk_delay() {
    Pair pair;
    auto sender = std::make_unique<cclient::SendQueue>(pair.client, 8, 1024, 1, 5000);
    check(sender->push(Bytes(10, 'X')), "Cannot enqueue delayed frame");
    receive(pair.peer, 1);
    const auto started = Clock::now();
    sender.reset();
    check(Clock::now() - started < std::chrono::seconds(1), "Chunk delay was not interrupted");
}

void send_failure() {
    cnet::Socket invalid;
    cclient::SendQueue sender(invalid);
    check(sender.push(Bytes{1, 2, 3}), "Cannot enqueue socket-error probe");
    wait_for([&] { return sender.failed() && sender.dropped() == 1; });
    check(!sender.push(Bytes{4}), "Failed queue accepted another frame");
}

void stop_idle() {
    Pair pair;
    const auto started = Clock::now();
    { cclient::SendQueue sender(pair.client); }
    check(Clock::now() - started < std::chrono::seconds(1), "Idle worker was not woken for shutdown");
}

void drain_completes_and_closes_admission() {
    Pair pair;
    std::vector<cclient::SendCompletion> completed;
    auto now = [] { return static_cast<mavlink_gateway::MonoNs>(std::chrono::duration_cast<std::chrono::nanoseconds>(
        Clock::now().time_since_epoch()).count()); };
    cclient::SendQueue sender(pair.client, 8, 1024, 1, 1, now,
        [&](const cclient::SendCompletion& result) { completed.push_back(result); });
    check(sender.push(Bytes(10, 'A'), false, {1, now()}), "Cannot enqueue tagged frame");
    check(sender.push(Bytes(5, 'B'), true, {2, now()}), "Cannot enqueue tagged priority frame");
    check(sender.drain_until(Clock::now() + std::chrono::seconds(1)), "Drain did not finish");
    check(!sender.push(Bytes{1}), "Draining queue still accepts new frames");
    check(completed.size() == 2, "Drain returned before completion callbacks");
    for (const auto& result : completed)
        check(result.status == mavlink_gateway::DeliveryStatus::SentToSocket &&
              result.completed_at_ns >= result.tag.queued_at_ns, "Invalid completion timing/status");
    check(sender.snapshot().pending_frames == 0 && sender.snapshot().socket_bytes == 15,
          "Drain counters do not match complete socket writes");
    receive(pair.peer, 15);
}

void drain_deadline_cancels_pending() {
    Pair pair;
    std::vector<cclient::SendCompletion> completed;
    cclient::SendQueue sender(pair.client, 8, 1024, 1, 5000, [] { return 100ULL; },
        [&](const cclient::SendCompletion& result) { completed.push_back(result); });
    check(sender.push(Bytes(10, 'X'), false, {7, 1}), "Cannot enqueue drain timeout probe");
    receive(pair.peer, 1);
    check(!sender.drain_until(Clock::now() + std::chrono::milliseconds(20)), "Drain ignored its deadline");
    check(sender.snapshot().pending_frames == 1, "In-flight frame missing from pending count");
    sender.close();
    check(completed.size() == 1 && completed[0].tag.id == 7 &&
          completed[0].status == mavlink_gateway::DeliveryStatus::ShutdownDiscard,
          "Cancelled frame did not get one terminal outcome");
    check(sender.snapshot().socket_bytes == 1 && sender.bytes_sent() == 0,
          "Partial bytes were confused with a complete frame");
}
} // namespace

int main() {
    // A regression must fail promptly instead of leaving the test process stuck in join/recv.
    std::mutex mutex;
    std::condition_variable wake;
    bool finished = false;
    std::thread watchdog([&] {
        std::unique_lock<std::mutex> lock(mutex);
        if (!wake.wait_for(lock, std::chrono::seconds(10), [&] { return finished; })) {
            std::fprintf(stderr, "FAIL: transport tests hung\n");
            std::fflush(stderr);
            std::_Exit(1);
        }
    });
    cnet::WinsockInit winsock;
    const struct { const char* name; void (*run)(); } cases[] = {
        {"priority_between_frames", priority_between_frames},
        {"stop_blocked_send", stop_blocked_send},
        {"stop_chunk_delay", stop_chunk_delay},
        {"send_failure", send_failure},
        {"stop_idle", stop_idle},
        {"drain_completes_and_closes_admission", drain_completes_and_closes_admission},
        {"drain_deadline_cancels_pending", drain_deadline_cancels_pending},
    };
    unsigned passed = 0;
    for (const auto& test : cases) {
        try { test.run(); ++passed; std::printf("PASS %s\n", test.name); }
        catch (const std::exception& error) { std::printf("FAIL %s: %s\n", test.name, error.what()); }
        std::fflush(stdout);
    }
    { std::lock_guard<std::mutex> lock(mutex); finished = true; }
    wake.notify_all();
    watchdog.join();
    std::printf("%u/7 transport tests passed.\n", passed);
    return passed == 7 ? 0 : 1;
}
