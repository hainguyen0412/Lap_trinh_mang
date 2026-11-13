#include "mavlink_gateway/processing.hpp"
#include "mavlink_gateway/csv_logger.hpp"
#include "client_config.hpp"
#include "net_socket.hpp"
#include "send_queue.hpp"
#include "mavlink_builder.hpp"
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <fstream>
#include <iostream>
#include <memory>
#include <mutex>
#include <sstream>
#include <thread>
#include <windows.h>
using namespace mavlink_gateway;
using Clock=std::chrono::steady_clock;
const auto origin=Clock::now();
MonoNs now_ns(){return std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now()-origin).count();}
volatile LONG console_stop=0;
BOOL WINAPI control(DWORD type){if(type==CTRL_C_EVENT||type==CTRL_BREAK_EVENT){InterlockedExchange(&console_stop,1);return TRUE;}return FALSE;}
std::string describe(const DecodedMessage& m) {
    char b[256];
    if (auto* h = std::get_if<Heartbeat>(&m))
        std::snprintf(b, sizeof b, "HEARTBEAT type=%u autopilot=%u base_mode=%u status=%u", h->type, h->autopilot,
                      h->base_mode, h->system_status);
    else if (auto* s = std::get_if<SysStatus>(&m))
        std::snprintf(b, sizeof b, "SYS_STATUS pin=%.2f V, %d%%", s->battery_voltage_v.value_or(-1.0),
                      s->battery_remaining_percent.value_or(-1));
    else if (auto* t = std::get_if<Attitude>(&m))
        std::snprintf(b, sizeof b, "ATTITUDE roll=%.3f pitch=%.3f yaw=%.3f (rad)", t->roll_rad, t->pitch_rad, t->yaw_rad);
    else if (auto* p = std::get_if<GlobalPosition>(&m))
        std::snprintf(b, sizeof b, "POSITION lat=%.6f lon=%.6f alt_msl=%.2f m rel=%.2f m", p->latitude_deg,
                      p->longitude_deg, p->altitude_msl_m, p->relative_altitude_m);
    else if (auto* k = std::get_if<CommandAck>(&m))
        std::snprintf(b, sizeof b, "COMMAND_ACK command=%u result=%u progress=%u", k->command, k->result, k->progress);
    else b[0] = 0;
    return b;
}


int run_client(const cclient::ClientOptions& a){
if(a.controller) throw std::invalid_argument("Controller is introduced in phase 12; use --role observer");
    cnet::WinsockInit winsock;SetConsoleCtrlHandler(control,TRUE);
    std::string error;auto socket=cnet::Socket::tcp_connect(a.host,a.port,&error);
    if(!socket.valid())throw std::runtime_error(error);
    LoggerOptions o;o.root_directory=a.shared.output_root;o.run_id=a.run_id;o.component=a.name;o.mode=a.mode;o.seed=a.shared.seed;
    o.queue_records=a.shared.logger_records;o.max_record_bytes=a.shared.log_record_bytes;o.max_file_bytes=a.shared.log_file_bytes;o.max_run_bytes=a.shared.log_run_bytes;
    o.flush_interval_ms=a.shared.log_flush_ms;o.shutdown_drain_ms=a.shared.log_drain_ms;
    if(a.resolved_config_json.size()+1>o.max_file_bytes)throw std::runtime_error("Config snapshot exceeds log file budget");
    o.reserved_control_bytes=4096+a.resolved_config_json.size()+1;
    o.metadata={{"resolved_config_file","resolved_config.json"},{"scope","client_console"}};
    CsvLogger logger(o);
    {std::ofstream file(logger.directory()/"resolved_config.json",std::ios::binary);file.exceptions(std::ios::failbit|std::ios::badbit);file<<a.resolved_config_json<<'\n';}
    std::printf("Connected %s:%u; Log: %s\n",a.host.c_str(),a.port,logger.directory().string().c_str());std::fflush(stdout);
    auto event=[&](const std::string& kind,const std::string& detail=""){logger.record_fields(LogTable::Events,{{"event_ns",std::to_string(now_ns())},{"event",kind},{"detail",detail}});};
    event("connected");
    Parser parser(a.shared.tcp_buffer_bytes);Decoder decoder;Inspector inspector;
    std::atomic<bool> running{true};

    std::uint8_t bytes[4096];FrameId next_id=1;
    while(running&&!InterlockedCompareExchange(&console_stop,0,0)){
        if(!socket.readable(100))continue;
        const auto count=socket.recv_some(bytes,sizeof(bytes));if(count<=0)break;
        auto batch=parser.consume({{1,1,Transport::Tcp,Flow::TelemetryDownlink,{a.host,a.port}},now_ns(),Bytes(bytes,bytes+count)});
        for(const auto& issue:batch.issues){if(a.mode!=MeasurementMode::B0)inspector.on_issue(issue);logger.record_issue(issue);}
        for(auto& frame:batch.frames){
            frame.frame_id=next_id++;logger.record_frame(frame);
            std::optional<DecodedMessage> decoded;
            if(a.mode!=MeasurementMode::B0||frame.header.msgid==77)decoded=decoder.decode(frame);
            if(a.mode!=MeasurementMode::B0)inspector.on_frame(frame,decoded);

            if(decoded)std::printf("%s\n",describe(*decoded).c_str());
        }
        if(batch.close_tcp_session)break;
    }
    running=false;

    socket.shutdown_both();socket.close();event("connection_closed");logger.close();
    const int result=logger.stats().last_error.empty()?0:1;
    std::fflush(stdout);
    if(a.controller)std::_Exit(result);
    return result;
}
int main(int argc,char** argv){
    cclient::ClientOptions options;
    try{options=cclient::load_client_options(argc,argv);}catch(const std::exception& e){std::cerr<<"Config/CLI error: "<<e.what()<<'\n';return 2;}
    if(options.help){std::cout<<cclient::client_usage();return 0;}
    if(options.print_config){std::cout<<options.resolved_config_json<<'\n';return 0;}
    try{return run_client(options);}catch(const std::exception& e){std::cerr<<"ERROR: "<<e.what()<<'\n';return 1;}
}
