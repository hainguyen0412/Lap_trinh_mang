#include "net_socket.hpp"
#include <cstring>
#ifdef _WIN32
using SockLen = int;
#else
#include <arpa/inet.h>
#include <errno.h>
#include <netdb.h>
#include <netinet/tcp.h>
#include <unistd.h>
using SockLen = socklen_t;
#endif

namespace cnet {
WinsockInit::WinsockInit() {
#ifdef _WIN32
    WSADATA d;
    WSAStartup(MAKEWORD(2, 2), &d);
#endif
}
WinsockInit::~WinsockInit() {
#ifdef _WIN32
    WSACleanup();
#endif
}

int last_error() {
#ifdef _WIN32
    return WSAGetLastError();
#else
    return errno;
#endif
}

static void close_handle(SocketHandle h) {
#ifdef _WIN32
    closesocket(h);
#else
    ::close(h);
#endif
}

static bool resolve(const std::string& host, int port, sockaddr_in& out, std::string* err) {
    addrinfo hints{};
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_STREAM;
    addrinfo* res = nullptr;
    if (getaddrinfo(host.c_str(), std::to_string(port).c_str(), &hints, &res) != 0 || !res) {
        if (err) *err = "khong phan giai duoc dia chi " + host;
        return false;
    }
    std::memcpy(&out, res->ai_addr, sizeof(sockaddr_in));
    freeaddrinfo(res);
    return true;
}

Socket& Socket::operator=(Socket&& o) noexcept {
    if (this != &o) {
        close();
        h_ = o.h_;
        o.h_ = kInvalid;
    }
    return *this;
}

void Socket::close() {
    if (h_ != kInvalid) {
        close_handle(h_);
        h_ = kInvalid;
    }
}

void Socket::shutdown_both() {
    if (h_ != kInvalid) ::shutdown(h_, 2); // 2 = SD_BOTH (Windows) = SHUT_RDWR (Linux)
}

bool Socket::send_all(const std::uint8_t* data, std::size_t len,
                      std::atomic<std::uint64_t>* accepted_bytes) {
    int flags = 0;
#ifdef MSG_NOSIGNAL
    flags = MSG_NOSIGNAL;
#endif
    std::size_t off = 0;
    while (off < len) {
        const auto n = ::send(h_, reinterpret_cast<const char*>(data) + off,
                              static_cast<int>(len - off), flags);
        if (n <= 0) return false;
        if (accepted_bytes) *accepted_bytes += static_cast<std::uint64_t>(n);
        off += static_cast<std::size_t>(n);
    }
    return true;
}

std::string Socket::peer_name() const {
    sockaddr_in address{};
    SockLen length = sizeof address;
    if (::getpeername(h_, reinterpret_cast<sockaddr*>(&address), &length) != 0) return {};
    char ip[64]{};
    if (!inet_ntop(AF_INET, &address.sin_addr, ip, sizeof ip)) return {};
    return std::string(ip) + ":" + std::to_string(ntohs(address.sin_port));
}

bool Socket::readable(int timeout_ms) const {
    fd_set readers;
    FD_ZERO(&readers); FD_SET(h_, &readers);
    timeval timeout{timeout_ms / 1000, (timeout_ms % 1000) * 1000};
#ifdef _WIN32
    const auto result = ::select(0, &readers, nullptr, nullptr, &timeout);
#else
    const auto result = ::select(h_ + 1, &readers, nullptr, nullptr, &timeout);
#endif
    return result != 0; // Let recv report EOF/error; false means an idle timeout only.
}

long Socket::recv_some(std::uint8_t* buf, std::size_t len) {
    return static_cast<long>(::recv(h_, reinterpret_cast<char*>(buf), static_cast<int>(len), 0));
}

Socket Socket::tcp_connect(const std::string& host, int port, std::string* error) {
    sockaddr_in addr{};
    if (!resolve(host, port, addr, error)) return Socket();
    Socket s(::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP));
    if (!s.valid()) {
        if (error) *error = "socket() loi " + std::to_string(last_error());
        return Socket();
    }
    int one = 1;
    setsockopt(s.h_, IPPROTO_TCP, TCP_NODELAY, reinterpret_cast<const char*>(&one), sizeof(one));
    if (::connect(s.h_, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0) {
        if (error) *error = "connect() loi " + std::to_string(last_error());
        return Socket();
    }
    return s;
}

Socket Socket::tcp_listen(int port, std::string* error) {
    Socket s(::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP));
    if (!s.valid()) {
        if (error) *error = "socket() loi " + std::to_string(last_error());
        return Socket();
    }
    int one = 1;
    setsockopt(s.h_, SOL_SOCKET, SO_REUSEADDR, reinterpret_cast<const char*>(&one), sizeof(one));
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_ANY);
    addr.sin_port = htons(static_cast<std::uint16_t>(port));
    if (::bind(s.h_, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0 || ::listen(s.h_, 4) != 0) {
        if (error) *error = "bind/listen loi " + std::to_string(last_error());
        return Socket();
    }
    return s;
}

Socket Socket::accept_client(std::string* peer, std::string* error) {
    sockaddr_in a{};
    SockLen len = sizeof(a);
    Socket c(::accept(h_, reinterpret_cast<sockaddr*>(&a), &len));
    if (!c.valid()) {
        if (error) *error = "accept() loi " + std::to_string(last_error());
        return Socket();
    }
    if (peer) {
        char ip[64] = {};
        inet_ntop(AF_INET, &a.sin_addr, ip, sizeof(ip));
        *peer = std::string(ip) + ":" + std::to_string(ntohs(a.sin_port));
    }
    return c;
}

UdpSender::UdpSender(const std::string& host, int port, std::string* error) {
    if (!resolve(host, port, addr_, error)) return;
    sock_ = Socket(::socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP));
    if (!sock_.valid() && error) *error = "socket() loi " + std::to_string(last_error());
}

bool UdpSender::send(const std::uint8_t* data, std::size_t len) {
    if (!sock_.valid()) return false;
    const int n = static_cast<int>(::sendto(sock_.handle(), reinterpret_cast<const char*>(data),
                                            static_cast<int>(len), 0,
                                            reinterpret_cast<const sockaddr*>(&addr_), sizeof(addr_)));
    return n == static_cast<int>(len);
}
} // namespace cnet
