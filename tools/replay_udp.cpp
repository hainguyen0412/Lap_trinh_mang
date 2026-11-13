// Bo phat lai UDP: doc file .bin (cac khung MAVLink noi tiep), tach tung khung bang Parser cua B,
// gui MOI KHUNG MOT DATAGRAM toi gateway (mac dinh 127.0.0.1:14550).
//   replay_udp <file.bin> [--host 127.0.0.1] [--port 14550] [--rate 200] [--seconds 70]
// --seconds 0 (mac dinh): phat 1 luot roi dung. --seconds N: lap lai file cho du N giay.
// Khung dau tien nen la HEARTBEAT dung sysid/compid cua PX4 de gateway khoa nguon.
// Cuoi cung in toc do phat THUC TE; bao cao phai dung so nay, khong dung --rate.
// Luu y: lap lai file lam SEQ bi lap -> Inspector se dem repeated_seq (dung ky vong, khong phai loi).
#include "mavlink_gateway/processing.hpp"
#include "net_socket.hpp"
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <iterator>
#include <string>
#include <thread>
#include <vector>

using namespace mavlink_gateway;
using Clock = std::chrono::steady_clock;

int main(int argc, char** argv) {
    if (argc < 2) { std::printf("Dung: replay_udp <file.bin> [--host H] [--port P] [--rate R] [--seconds S]\n"); return 2; }
    std::string file = argv[1], host = "127.0.0.1";
    int port = 14550;
    double rate = 200, seconds = 0;
    for (int i = 2; i < argc; ++i) {
        const std::string s = argv[i];
        if (s == "--host" && i + 1 < argc) host = argv[++i];
        else if (s == "--port" && i + 1 < argc) port = std::atoi(argv[++i]);
        else if (s == "--rate" && i + 1 < argc) rate = std::atof(argv[++i]);
        else if (s == "--seconds" && i + 1 < argc) seconds = std::atof(argv[++i]);
    }
    if (rate <= 0) { std::printf("--rate phai > 0\n"); return 2; }

    std::ifstream in(file, std::ios::binary);
    if (!in) { std::printf("Khong mo duoc %s\n", file.c_str()); return 1; }
    Bytes data((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());

    // Tach file thanh tung khung (dung Parser che do TCP nhu mot luong byte).
    Parser parser;
    ReceiveEvent ev;
    ev.source = {1, 1, Transport::Tcp, Flow::TelemetryDownlink, {"file", 0}};
    ev.bytes = data;
    ParseBatch batch = parser.consume(ev);
    std::vector<Bytes> frames;
    for (const auto& f : batch.frames) frames.push_back(f.raw);
    if (frames.empty()) { std::printf("Khong tach duoc khung hop le nao tu %s\n", file.c_str()); return 1; }
    for (const auto& i : batch.issues) std::printf("Canh bao khi tach file: %s\n", issue_name(i.code));
    const auto& first = batch.frames.front();
    std::printf("Tach duoc %zu khung. Khung dau: msgid=%u sysid=%u compid=%u %s\n", frames.size(), first.header.msgid,
                first.header.sysid, first.header.compid,
                first.header.msgid == 0 ? "(HEARTBEAT, tot)" : "(CANH BAO: khung dau khong phai HEARTBEAT, gateway se chua khoa nguon)");

    cnet::WinsockInit winsock;
    std::string err;
    cnet::UdpSender udp(host, port, &err);
    if (!udp.ok()) { std::printf("Loi UDP: %s\n", err.c_str()); return 1; }
    std::printf("Phat toi %s:%d, toc do muc tieu %.1f khung/s%s\n", host.c_str(), port, rate,
                seconds > 0 ? "" : " (1 luot)");

    const auto interval = std::chrono::duration<double>(1.0 / rate);
    const auto t0 = Clock::now();
    std::uint64_t sent = 0, failed = 0;
    std::size_t idx = 0;
    for (;;) {
        std::this_thread::sleep_until(t0 + std::chrono::duration_cast<Clock::duration>(interval * sent));
        const double elapsed = std::chrono::duration<double>(Clock::now() - t0).count();
        if (seconds > 0 && elapsed >= seconds) break;
        if (udp.send(frames[idx].data(), frames[idx].size())) ++sent; else { ++failed; ++sent; }
        if (++idx == frames.size()) {
            idx = 0;
            if (seconds <= 0) break;
        }
    }
    const double total = std::chrono::duration<double>(Clock::now() - t0).count();
    std::printf("Da gui %llu khung (loi %llu) trong %.3f s -> toc do THUC TE %.1f khung/s\n", (unsigned long long)sent,
                (unsigned long long)failed, total, total > 0 ? sent / total : 0.0);
    return 0;
}
