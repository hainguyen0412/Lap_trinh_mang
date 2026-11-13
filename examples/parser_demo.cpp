#include "mavlink_gateway/csv_logger.hpp"
#include <chrono>
#include <fstream>
#include <iostream>
#include <iterator>
#include <stdexcept>
#include <type_traits>

using namespace mavlink_gateway;
using Clock=std::chrono::steady_clock;
void display(const DecodedMessage& decoded){
    std::visit([](const auto& value){using T=std::decay_t<decltype(value)>;
        if constexpr(std::is_same_v<T,Heartbeat>)std::cout<<" HEARTBEAT mode="<<value.custom_mode<<" state="<<unsigned(value.system_status);
        else if constexpr(std::is_same_v<T,SysStatus>)std::cout<<" SYS_STATUS voltage="<<(value.battery_voltage_v?std::to_string(*value.battery_voltage_v):"unknown")<<"V battery="<<(value.battery_remaining_percent?std::to_string(*value.battery_remaining_percent):"unknown")<<"%";
        else if constexpr(std::is_same_v<T,Attitude>)std::cout<<" ATTITUDE roll="<<value.roll_rad<<" pitch="<<value.pitch_rad<<" yaw="<<value.yaw_rad<<" rad";
        else if constexpr(std::is_same_v<T,GlobalPosition>)std::cout<<" POSITION lat="<<value.latitude_deg<<" lon="<<value.longitude_deg<<" altitude="<<value.altitude_msl_m<<"m relative="<<value.relative_altitude_m<<'m';
        else if constexpr(std::is_same_v<T,CommandAck>)std::cout<<" ACK command="<<value.command<<" result="<<unsigned(value.result);
    },decoded);
}
int main(int argc,char** argv){
    try{
        const Clock::time_point origin=Clock::now();
        const std::string input=argc>1?argv[1]:"tests/fixtures/v1/valid.bin";
        const std::string mode=argc>2?argv[2]:"B1";
        const std::string transport=argc>3?argv[3]:"tcp";
        if(mode!="B0"&&mode!="B1"&&mode!="B2")throw std::invalid_argument("Mode must be B0, B1 or B2");
        if(transport!="tcp"&&transport!="udp")throw std::invalid_argument("Transport must be tcp or udp");
        if(std::filesystem::file_size(input)>16*1024*1024)throw std::invalid_argument("Offline demo input limit: 16 MiB");
        std::ifstream file(input,std::ios::binary);if(!file)throw std::runtime_error("Cannot open input "+input);
        Bytes bytes(std::istreambuf_iterator<char>(file),{});
        LoggerOptions options;options.mode=mode=="B0"?MeasurementMode::B0:(mode=="B1"?MeasurementMode::B1:MeasurementMode::B2);
        options.run_id=argc>4?argv[4]:"offline_b_"+std::to_string(origin.time_since_epoch().count());
        options.component="b_demo";options.scope="offline_demo";options.metadata={{"input",input},{"transport",transport},{"network","none"},{"px4_version","not_used"}};
        CsvLogger logger(options);Parser parser;Decoder decoder;Inspector inspector;
        std::uint64_t frames=0,issues=0;
        const auto started=Clock::now();
        for(std::size_t offset=0;offset<bytes.size();){
            const auto amount=transport=="udp"?bytes.size():std::min<std::size_t>(7,bytes.size()-offset);
            ReceiveEvent event;
            event.source={1,transport=="tcp"?1U:0U,transport=="tcp"?Transport::Tcp:Transport::Udp,Flow::TelemetryDownlink,{"fixture",14580}};
            event.received_at_ns=static_cast<MonoNs>(std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now()-origin).count());
            event.bytes.assign(bytes.begin()+static_cast<std::ptrdiff_t>(offset),bytes.begin()+static_cast<std::ptrdiff_t>(offset+amount));offset+=amount;
            auto batch=parser.consume(event);
            for(const auto& problem:batch.issues){++issues;std::cout<<"issue="<<issue_name(problem.code)<<" discarded="<<problem.discarded_bytes<<'\n';
                if(options.mode!=MeasurementMode::B0)inspector.on_issue(problem);
                if(options.mode==MeasurementMode::B2)logger.record_issue(problem);
            }
            for(auto& frame:batch.frames){frame.frame_id=++frames;
                std::cout<<"#"<<frame.frame_id<<" msgid="<<frame.header.msgid<<" seq="<<unsigned(frame.header.seq)<<" bytes="<<frame.raw.size();
                if(options.mode!=MeasurementMode::B0){auto decoded=decoder.decode(frame);inspector.on_frame(frame,decoded);if(decoded)display(*decoded);}
                std::cout<<'\n';if(options.mode==MeasurementMode::B2)logger.record_frame(frame);
            }
            if(batch.close_tcp_session)break;
        }
        const double seconds=std::chrono::duration<double>(Clock::now()-started).count();
        auto snapshot=inspector.snapshot();
        std::map<std::string,std::string> metrics{{"sample_ns",std::to_string(std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now()-origin).count())},{"valid_total",std::to_string(frames)},{"active_clients","0"},{"log_drop",std::to_string(logger.stats().dropped)}};
        if(options.mode!=MeasurementMode::B0){std::uint64_t gap=0,repeat=0,ambiguous=0;for(const auto& s:snapshot.sources){gap+=s.estimated_missing;repeat+=s.repeated_seq;ambiguous+=s.ambiguous_seq;}
            metrics.insert({{"bad_crc_total",std::to_string(snapshot.bad_crc_candidates)},{"unsupported_total",std::to_string(snapshot.unsupported_candidates)},{"seq_gap_total",std::to_string(gap)},{"seq_repeat_total",std::to_string(repeat)},{"seq_ambiguous_total",std::to_string(ambiguous)}});
            std::cout<<"Inspector: valid="<<snapshot.valid_frames<<" sources="<<snapshot.sources.size()<<" estimated_missing="<<gap<<'\n';
        }
        logger.record_fields(LogTable::Metrics,metrics);
        logger.set_summary({{"clients","0"},{"measurement_s",std::to_string(seconds)},{"valid_window",std::to_string(frames)}});
        logger.close();const auto log_stats=logger.stats();
        std::cout<<"Offline demo: frames="<<frames<<" issues="<<issues<<" mode="<<mode<<" transport="<<transport<<"\nLogs: "<<logger.directory().string()<<'\n';
        if(!log_stats.last_error.empty())throw std::runtime_error(log_stats.last_error);
        return 0;
    }catch(const std::exception& e){std::cerr<<"Error: "<<e.what()<<"\nUsage: b_demo.exe [input.bin] [B0|B1|B2] [tcp|udp] [unique_run_id]\n";return 1;}
}
