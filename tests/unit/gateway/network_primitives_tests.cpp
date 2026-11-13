#include "network.hpp"
#include "bounded_queue.hpp"
#include <future>
#include <iostream>

namespace {
void check(bool ok, const char* message) { if (!ok) throw std::runtime_error(message); }

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


        auto chunked=mavlink_gateway::send_fully(input,[](const auto*,int n){check(n<=3,"chunk limit");return n;},[]{return true;},[]{return false;},3);
        check(chunked.complete&&chunked.bytes==input.size(),"bounded write chunks");
        int interruptions=0;
        auto interrupted=mavlink_gateway::send_fully(input,[&](const auto*,int n){
            if(++interruptions==1){WSASetLastError(WSAEINTR);return SOCKET_ERROR;}return n;
        },[]{return true;},[]{return false;});
        check(interrupted.complete&&interruptions==2,"retry interrupted write");
        auto oversize=mavlink_gateway::send_fully(input,[](const auto*,int n){return n+1;},[]{return true;},[]{return false;});
        check(!oversize.complete&&oversize.bytes==0&&oversize.error=="invalid_write_result","reject impossible write result");
        int partial_calls=0;
        auto partial=mavlink_gateway::send_fully(input,[&](const auto*,int){++partial_calls;return 2;},[]{return true;},[&]{return partial_calls>0;});
        check(!partial.complete&&partial.bytes==2&&partial.error=="cancelled","preserve partial bytes on cancellation");
        auto fatal=mavlink_gateway::send_fully(input,[](const auto*,int){WSASetLastError(WSAECONNRESET);return SOCKET_ERROR;},[]{return true;},[]{return false;});
        check(!fatal.complete&&!fatal.error.empty(),"fatal socket error terminates send");
        auto empty=mavlink_gateway::send_fully({},[](const auto*,int)->int{throw std::runtime_error("unexpected empty write");},[]{return true;},[]{return false;});
        check(empty.complete&&empty.bytes==0,"empty frame needs no socket write");
        std::cout<<"PASS bounded chunks, interrupt, invalid result, partial cancel, fatal error, empty write\n";

        auto udp=mavlink_gateway::bound_socket({"127.0.0.1",0},false);
        sockaddr_in bound{};int size=sizeof(bound);
        check(getsockname(udp.get(),reinterpret_cast<sockaddr*>(&bound),&size)==0,"UDP getsockname");
        const auto local=mavlink_gateway::endpoint(bound);
        check(local.ip=="127.0.0.1"&&local.port!=0,"UDP endpoint conversion");
        bool refused=false;
        try{auto collision=mavlink_gateway::bound_socket(local,false);}catch(const std::exception&){refused=true;}
        check(refused,"exclusive bind rejects occupied UDP port");
        const SOCKET handle=udp.get();
        mavlink_gateway::SocketHandle moved(std::move(udp));
        check(udp.get()==INVALID_SOCKET&&moved.get()==handle,"move transfers socket ownership");
        moved.reset();int type=0;int type_size=sizeof(type);
        check(getsockopt(handle,SOL_SOCKET,SO_TYPE,reinterpret_cast<char*>(&type),&type_size)==SOCKET_ERROR&&WSAGetLastError()==WSAENOTSOCK,"reset closes owned socket");
        auto tcp=mavlink_gateway::bound_socket({"127.0.0.1",0},true);
        check(!mavlink_gateway::ready(tcp.get(),false,1),"idle TCP listener does not report data");
        bool invalid=false;try{mavlink_gateway::address({"invalid-ip",14550});}catch(const std::exception&){invalid=true;}
        check(invalid,"invalid IPv4 rejected");
        std::cout<<"PASS UDP/TCP bind, exclusive port, endpoint, move/reset and invalid IPv4\n";
        std::cout<<"4/4 network primitive groups passed\n";return 0;
    }catch(const std::exception& e){std::cerr<<"FAIL: "<<e.what()<<'\n';return 1;}
}
