#pragma once
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <winsock2.h>
#include <ws2tcpip.h>
#include "mavlink_gateway/contracts.hpp"
#include <atomic>
#include <functional>
#include <stdexcept>

namespace mavlink_gateway {
inline bool same_endpoint(const Endpoint& a,const Endpoint& b){return a.ip==b.ip&&a.port==b.port;}
class WinsockRuntime {
public:
    WinsockRuntime() { WSADATA data{}; int e = WSAStartup(MAKEWORD(2, 2), &data); if (e) throw std::runtime_error("WSAStartup: " + std::to_string(e)); }
    ~WinsockRuntime() { WSACleanup(); }
    WinsockRuntime(const WinsockRuntime&) = delete;
    WinsockRuntime& operator=(const WinsockRuntime&) = delete;
};
class SocketHandle {
    SOCKET value_ = INVALID_SOCKET;
public:
    SocketHandle() = default;
    explicit SocketHandle(SOCKET s) : value_(s) {}
    ~SocketHandle() { reset(); }
    SocketHandle(const SocketHandle&) = delete;
    SocketHandle& operator=(const SocketHandle&) = delete;
    SocketHandle(SocketHandle&& b) noexcept : value_(b.value_) { b.value_ = INVALID_SOCKET; }
    SocketHandle& operator=(SocketHandle&& b) noexcept {
        if (this != &b) { reset(); value_ = b.value_; b.value_ = INVALID_SOCKET; } return *this;
    }
    SOCKET get() const { return value_; }
    void reset() { if (value_ != INVALID_SOCKET) { closesocket(value_); value_ = INVALID_SOCKET; } }
};
inline sockaddr_in address(const Endpoint& e) {
    sockaddr_in a{}; a.sin_family = AF_INET; a.sin_port = htons(e.port);
    if (inet_pton(AF_INET, e.ip.c_str(), &a.sin_addr) != 1) throw std::runtime_error("Invalid IPv4: " + e.ip);
    return a;
}
inline Endpoint endpoint(const sockaddr_in& a) {
    char text[INET_ADDRSTRLEN]{}; inet_ntop(AF_INET, &a.sin_addr, text, sizeof(text));
    return {text, ntohs(a.sin_port)};
}
inline bool ready(SOCKET s, bool writing, int ms = 50) {
    fd_set fds; FD_ZERO(&fds); FD_SET(s, &fds); timeval timeout{ms / 1000, (ms % 1000) * 1000};
    int n = select(0, writing ? nullptr : &fds, writing ? &fds : nullptr, nullptr, &timeout);
    if (n == SOCKET_ERROR) throw std::runtime_error("select: " + std::to_string(WSAGetLastError()));
    return n > 0;
}
SocketHandle bound_socket(const Endpoint&, bool tcp);
struct SendResult { std::size_t bytes = 0; bool complete = false; std::string error; };
// Injectable write/wait functions allow a deterministic partial-send unit test.
SendResult send_fully(const Bytes&, const std::function<int(const std::uint8_t*, int)>& write,
                      const std::function<bool()>& wait, const std::function<bool()>& cancelled,
                      std::size_t chunk_limit = 0);
} // namespace mavlink_gateway
