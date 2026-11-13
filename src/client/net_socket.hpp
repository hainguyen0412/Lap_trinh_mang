#pragma once
// Socket TCP/UDP boc RAII. Chay tren Windows (Winsock) va Linux (de test).
#include <cstddef>
#include <cstdint>
#include <string>
#include <atomic>
#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
#else
#include <netinet/in.h>
#include <sys/socket.h>
#endif

namespace cnet {
#ifdef _WIN32
using SocketHandle = SOCKET;
inline const SocketHandle kInvalid = INVALID_SOCKET;
#else
using SocketHandle = int;
inline const SocketHandle kInvalid = -1;
#endif

// Tao 1 doi tuong o dau main(): WSAStartup/WSACleanup tren Windows, khong lam gi tren Linux.
struct WinsockInit {
    WinsockInit();
    ~WinsockInit();
};

int last_error();

class Socket {
public:
    Socket() = default;
    explicit Socket(SocketHandle h) : h_(h) {}
    ~Socket() { close(); }
    Socket(const Socket&) = delete;
    Socket& operator=(const Socket&) = delete;
    Socket(Socket&& o) noexcept : h_(o.h_) { o.h_ = kInvalid; }
    Socket& operator=(Socket&& o) noexcept;

    bool valid() const { return h_ != kInvalid; }
    SocketHandle handle() const { return h_; }
    void close();
    // Danh thuc thread dang bi chan trong recv/send. Goi truoc close().
    void shutdown_both();
    // Gui du len bytes (lap lai neu send chi gui duoc 1 phan). false = loi/dong.
    bool send_all(const std::uint8_t* data, std::size_t len,
                  std::atomic<std::uint64_t>* accepted_bytes = nullptr);
    std::string peer_name() const;
    bool readable(int timeout_ms) const;
    // >0: so byte nhan; 0: ben kia dong; <0: loi.
    long recv_some(std::uint8_t* buf, std::size_t len);

    static Socket tcp_connect(const std::string& host, int port, std::string* error);
    static Socket tcp_listen(int port, std::string* error);
    Socket accept_client(std::string* peer, std::string* error);

private:
    SocketHandle h_ = kInvalid;
};

class UdpSender {
public:
    UdpSender(const std::string& host, int port, std::string* error);
    bool ok() const { return sock_.valid(); }
    bool send(const std::uint8_t* data, std::size_t len);

private:
    Socket sock_;
    sockaddr_in addr_{};
};
} // namespace cnet
