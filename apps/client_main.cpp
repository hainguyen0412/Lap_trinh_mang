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

    std::unique_ptr<cclient::SendQueue> sender;
    std::unique_ptr<cclient::CommandManager> manager;
    std::mutex command_mutex;
    std::thread ticker;
    if(a.controller){
        sender=std::make_unique<cclient::SendQueue>(socket,a.shared.client_queue_frames,a.shared.client_queue_bytes);
        const cclient::GcsId gcs{a.command.gcs_sysid,a.command.gcs_compid};
        const auto cfg=a.command;
        manager=std::make_unique<cclient::CommandManager>(cfg,
            [gcs,cfg](std::uint16_t id,const cclient::Params& params,std::uint8_t confirmation){return cclient::build_command_long(gcs,cfg.px4_sysid,cfg.px4_compid,id,params,confirmation);},
            [&](const Bytes& raw){return sender->push(raw);},
            [&](const cclient::CommandEvent& e){
                std::map<std::string,std::string> fields{{"request_id",std::to_string(e.request_id)},{"session_id","1"},{"command",std::to_string(e.command)},
                    {"target_system",std::to_string(e.target_system)},{"target_component",std::to_string(e.target_component)},{"attempt",std::to_string(e.attempt)},
                    {"event_ns",std::to_string(e.at_ns)},{"event",e.event}};
                if(e.result)fields["result"]=std::to_string(*e.result);
                if(e.progress)fields["progress"]=std::to_string(*e.progress);
                logger.record_fields(LogTable::Commands,fields);
            });
        ticker=std::thread([&,gcs]{
            auto next=Clock::now();const auto period=std::chrono::milliseconds(a.heartbeat_interval_ms);
            const auto tick=std::chrono::milliseconds(std::min<unsigned>(50,a.heartbeat_interval_ms));
            while(running){
                {std::lock_guard<std::mutex> lock(command_mutex);manager->tick(now_ns());}
                if(Clock::now()>=next){sender->push(cclient::build_gcs_heartbeat(gcs));next=Clock::now()+period;}
                std::this_thread::sleep_until(std::min(next,Clock::now()+tick));
            }
        });
        std::thread([&]{
            std::puts("Lenh: arm | disarm | takeoff [do_cao_amsl_m] | land | help | quit");
            std::string line;
            while(running&&std::getline(std::cin,line)){
                std::istringstream input(line);std::string word;input>>word;
                if(word=="quit"||word=="exit"){running=false;socket.shutdown_both();break;}
                cclient::Params params{};std::uint16_t id=0;
                if(word=="arm"){id=cclient::kCmdArm;params[0]=1;}
                else if(word=="disarm")id=cclient::kCmdArm;
                else if(word=="takeoff"){id=cclient::kCmdTakeoff;input>>params[6];}
                else if(word=="land")id=cclient::kCmdLand;
                else continue;
                if(std::find(a.shared.allowed_commands.begin(),a.shared.allowed_commands.end(),id)==a.shared.allowed_commands.end()){
                    std::printf("Command %u is disabled by config\n",id);event("command_not_allowed",std::to_string(id));continue;
                }
                std::lock_guard<std::mutex> lock(command_mutex);
                if(!running)break;
                manager->submit(id,params,now_ns());
            }
        }).detach();
    }

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
if(manager&&decoded){if(const auto* value=std::get_if<CommandAck>(&*decoded)){std::lock_guard<std::mutex> lock(command_mutex);manager->on_ack(*value,frame.header.sysid,frame.header.compid,frame.received_at_ns);}}
            if(decoded)std::printf("%s\n",describe(*decoded).c_str());
        }
        if(batch.close_tcp_session)break;
    }
    running=false;
if(manager){std::lock_guard<std::mutex> lock(command_mutex);manager->on_disconnect(now_ns());}
    if(ticker.joinable())ticker.join();
    if(sender){sender->drain_until(Clock::now()+std::chrono::milliseconds(a.shared.drain_ms));sender->close();}
    socket.shutdown_both();socket.close();event("connection_closed");logger.close();
    const int result=logger.stats().last_error.empty()?0:1;
    if(a.mode!=MeasurementMode::B0){
        const auto stats=inspector.snapshot();
        std::printf("== Thong ke: %llu khung hop le | bad_crc=%llu | unsupported=%llu\n",
            static_cast<unsigned long long>(stats.valid_frames),
            static_cast<unsigned long long>(stats.bad_crc_candidates),
            static_cast<unsigned long long>(stats.unsupported_candidates));
    }
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
