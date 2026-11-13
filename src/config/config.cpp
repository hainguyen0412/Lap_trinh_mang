#include "mavlink_gateway/config.hpp"
#include <nlohmann/json.hpp>
#include <algorithm>
#include <fstream>
#include <limits>
#include <set>
#include <stdexcept>

namespace mavlink_gateway {
namespace {
using Json=nlohmann::json;
void keys(const Json& value,const std::string& path,std::initializer_list<const char*> names){
    if(!value.is_object())throw std::runtime_error("Expected JSON object: "+path);
    std::set<std::string> allowed(names.begin(),names.end());
    for(auto it=value.begin();it!=value.end();++it)
        if(!allowed.count(it.key()))throw std::runtime_error("Unknown config key: "+path+"."+it.key());
}
std::uint64_t number(const Json& obj,const char* key,std::uint64_t minimum,std::uint64_t maximum){
    const auto& v=obj.at(key);
    if(!v.is_number_integer()||(!v.is_number_unsigned()&&v.get<std::int64_t>()<0))
        throw std::runtime_error(std::string("Expected nonnegative integer: ")+key);
    const auto n=v.get<std::uint64_t>();
    if(n<minimum||n>maximum)throw std::runtime_error(std::string("Out of range: ")+key);
    return n;
}
std::string text(const Json& obj,const char* key){
    const auto value=obj.at(key).get<std::string>();
    if(value.empty())throw std::runtime_error(std::string("Empty config text: ")+key);
    return value;
}
bool boolean(const Json& obj,const char* key){return obj.at(key).get<bool>();}
const Json& section(const Json& root,const char* key,std::initializer_list<const char*> names){
    const auto& value=root.at(key);keys(value,key,names);return value;
}
}
Config load_config(const std::string& path){
    std::ifstream input(path);if(!input)throw std::runtime_error("Cannot open config: "+path);
    Json j;input>>j;
    keys(j,"root",{"schema_version","network","px4","protocol","limits","client","commands","measurement","logging"});
    number(j,"schema_version",1,1);
    Config c;
    const auto& net=section(j,"network",{"udp_bind","tcp_observer_bind","tcp_controller_bind","max_observers","max_controllers"});
    auto bind=[&](const char* key,std::string& host,std::uint16_t& port){
        const auto& value=net.at(key);keys(value,key,{"host","port"});
        host=text(value,"host");port=static_cast<std::uint16_t>(number(value,"port",1,65535));
    };
    bind("udp_bind",c.udp_bind_host,c.udp_port);
    bind("tcp_observer_bind",c.observer_bind_host,c.observer_port);
    bind("tcp_controller_bind",c.controller_bind_host,c.controller_port);
    c.max_observers=number(net,"max_observers",1,9);number(net,"max_controllers",1,1);
    if(c.observer_port==c.controller_port&&c.observer_bind_host==c.controller_bind_host)
        throw std::runtime_error("TCP role endpoints must be different");
    const auto& px4=section(j,"px4",{"endpoint_policy","expected_sysid","expected_compid","heartbeat_stale_after_ms","reject_commands_when_stale"});
    if(text(px4,"endpoint_policy")!="lock_first_valid_matching_heartbeat")throw std::runtime_error("Unsupported PX4 endpoint policy");
    c.px4_sysid=static_cast<std::uint8_t>(number(px4,"expected_sysid",1,255));
    c.px4_compid=static_cast<std::uint8_t>(number(px4,"expected_compid",1,255));
    c.heartbeat_stale_ms=static_cast<int>(number(px4,"heartbeat_stale_after_ms",1,60000));
    c.reject_commands_when_stale=boolean(px4,"reject_commands_when_stale");
    const auto& protocol=section(j,"protocol",{"mavlink_version","dialect","allow_signed_frames","decode_msgids"});
    number(protocol,"mavlink_version",2,2);
    if(text(protocol,"dialect")!="common"||boolean(protocol,"allow_signed_frames"))throw std::runtime_error("Only unsigned MAVLink 2 common is supported");
    const auto& decoded=protocol.at("decode_msgids");
    if(!decoded.is_array()||decoded.size()!=5)throw std::runtime_error("decode_msgids must contain the five agreed messages");
    std::set<std::uint32_t> decoded_ids;
    for(const auto& id:decoded){Json temp={{"id",id}};decoded_ids.insert(static_cast<std::uint32_t>(number(temp,"id",0,16777215)));}
    if(decoded_ids!=std::set<std::uint32_t>{0,1,30,33,77})throw std::runtime_error("Unsupported decoder message set");
    const auto& limits=section(j,"limits",{"udp_receive_queue_datagrams","udp_send_queue_frames","tcp_parser_buffer_bytes","client_send_queue_frames","client_send_queue_bytes","logger_queue_records","udp_candidate_endpoints"});
    c.udp_queue_datagrams=number(limits,"udp_receive_queue_datagrams",1,4096);
    c.udp_uplink_frames=number(limits,"udp_send_queue_frames",1,256);
    c.tcp_buffer_bytes=number(limits,"tcp_parser_buffer_bytes",1,65536);
    c.client_queue_frames=number(limits,"client_send_queue_frames",1,1024);
    c.client_queue_bytes=number(limits,"client_send_queue_bytes",1,1048576);
    c.logger_records=number(limits,"logger_queue_records",1,10000);
    c.max_udp_candidates=number(limits,"udp_candidate_endpoints",1,16);
    const auto& client=section(j,"client",{"gateway_host","gcs_sysid","gcs_compid","heartbeat_interval_ms"});
    text(client,"gateway_host");number(client,"heartbeat_interval_ms",1,60000);
    c.gcs_sysid=static_cast<std::uint8_t>(number(client,"gcs_sysid",1,255));
    c.gcs_compid=static_cast<std::uint8_t>(number(client,"gcs_compid",1,255));
    const auto& command=section(j,"commands",{"allowed_command_ids","max_pending","ack_timeout_ms","max_total_attempts","in_progress_idle_timeout_ms","in_progress_total_timeout_ms"});
    const auto& commands=command.at("allowed_command_ids");
    if(!commands.is_array()||commands.empty()||commands.size()>3)throw std::runtime_error("Invalid allowed_command_ids");
    c.allowed_commands.clear();
    for(const auto& id:commands){Json temp={{"id",id}};auto n=number(temp,"id",1,65535);
        if((n!=400&&n!=22&&n!=21)||std::find(c.allowed_commands.begin(),c.allowed_commands.end(),n)!=c.allowed_commands.end())throw std::runtime_error("Unsupported/duplicate command ID");
        c.allowed_commands.push_back(static_cast<std::uint16_t>(n));}
    number(command,"max_pending",1,1);number(command,"ack_timeout_ms",1,60000);number(command,"max_total_attempts",1,3);
    const auto idle=number(command,"in_progress_idle_timeout_ms",1,600000);
    number(command,"in_progress_total_timeout_ms",idle,600000);
    const auto& measurement=section(j,"measurement",{"mode","seed","warmup_seconds","duration_seconds","drain_timeout_ms","snapshot_interval_ms","offered_frames_per_second","expected_clients"});
    c.mode=text(measurement,"mode");if(c.mode!="B0"&&c.mode!="B1"&&c.mode!="B2")throw std::runtime_error("mode must be B0/B1/B2");
    c.seed=static_cast<std::uint32_t>(number(measurement,"seed",0,std::numeric_limits<std::uint32_t>::max()));
    c.warmup_seconds=static_cast<int>(number(measurement,"warmup_seconds",0,3600));
    c.measurement_seconds=static_cast<int>(number(measurement,"duration_seconds",0,86400));
    c.drain_ms=static_cast<int>(number(measurement,"drain_timeout_ms",0,10000));
    c.snapshot_interval_ms=static_cast<int>(number(measurement,"snapshot_interval_ms",50,60000));
    number(measurement,"offered_frames_per_second",1,1000000);number(measurement,"expected_clients",1,c.max_observers+1);
    const auto& logging=section(j,"logging",{"schema_version","root_directory","flush_interval_ms","shutdown_drain_ms","max_record_bytes","write_frame_detail_only_in_B2","max_file_bytes","max_component_run_bytes"});
    number(logging,"schema_version",1,1);c.output_root=text(logging,"root_directory");
    c.log_flush_ms=static_cast<int>(number(logging,"flush_interval_ms",1,60000));
    c.log_drain_ms=static_cast<int>(number(logging,"shutdown_drain_ms",0,10000));
    c.log_record_bytes=number(logging,"max_record_bytes",256,1048576);
    c.log_file_bytes=number(logging,"max_file_bytes",1024,104857600);
    c.log_run_bytes=number(logging,"max_component_run_bytes",131072,536870912);
    if(!boolean(logging,"write_frame_detail_only_in_B2"))throw std::runtime_error("Frame detail is restricted to B2");
    c.resolved_config_json=j.dump();
    return c;
}
}
