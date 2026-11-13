#include "network.hpp"
#include <algorithm>
#include <limits>

namespace mavlink_gateway {
SocketHandle bound_socket(const Endpoint& e, bool tcp) {
    SocketHandle s(socket(AF_INET, tcp ? SOCK_STREAM : SOCK_DGRAM, tcp ? IPPROTO_TCP : IPPROTO_UDP));
    if (s.get() == INVALID_SOCKET) throw std::runtime_error("socket: " + std::to_string(WSAGetLastError()));
    // Refuse accidental sharing with another gateway on Windows.
    BOOL exclusive = TRUE;
    if (setsockopt(s.get(), SOL_SOCKET, SO_EXCLUSIVEADDRUSE, reinterpret_cast<const char*>(&exclusive), sizeof(exclusive)))
        throw std::runtime_error("SO_EXCLUSIVEADDRUSE failed");
    auto a = address(e);
    if (bind(s.get(), reinterpret_cast<sockaddr*>(&a), sizeof(a)))
        throw std::runtime_error("bind " + e.ip + ":" + std::to_string(e.port) + ": " + std::to_string(WSAGetLastError()));
    u_long mode = 1;
    if (ioctlsocket(s.get(), FIONBIO, &mode)) throw std::runtime_error("FIONBIO failed");
    if (tcp && listen(s.get(), SOMAXCONN)) throw std::runtime_error("listen failed");
    return s;
}
SendResult send_fully(const Bytes& bytes, const std::function<int(const std::uint8_t*, int)>& write,
                      const std::function<bool()>& wait, const std::function<bool()>& cancelled,
                      std::size_t chunk_limit) {
    SendResult result;
    while (result.bytes < bytes.size()) {
        if (cancelled()) { result.error = "cancelled"; return result; }
        if (!wait()) continue;
        auto left = std::min<std::size_t>(bytes.size() - result.bytes, std::numeric_limits<int>::max());
        if (chunk_limit) left = std::min(left, chunk_limit);
        const int n = write(bytes.data() + result.bytes, static_cast<int>(left));
        if (n > 0) {
            if (static_cast<std::size_t>(n) > left) { result.error = "invalid_write_result"; return result; }
            result.bytes += n;
        } else if (n == SOCKET_ERROR) {
            const int error = WSAGetLastError();
            if (error == WSAEWOULDBLOCK || error == WSAEINTR) continue;
            result.error = std::to_string(error); return result;
        } else { result.error = "zero_write"; return result; }
    }
    result.complete = true; return result;
}
} // namespace mavlink_gateway
