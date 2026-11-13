#include "mavlink_gateway/config.hpp"
#include <nlohmann/json.hpp>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iostream>
#include <stdexcept>

using Json=nlohmann::json;
namespace fs=std::filesystem;
void check(bool ok,const char* what){if(!ok)throw std::runtime_error(what);}
int main(int argc,char** argv){
    try{
        const fs::path sample=argc>1?argv[1]:"config/gateway.example.json";
        std::ifstream input(sample);Json base;input>>base;
        auto c=mavlink_gateway::load_config(sample.string());
        check(c.udp_port==14550&&c.observer_port==5760&&c.controller_port==5761,"default ports");
        check(c.mode=="B1"&&c.px4_sysid==1&&c.gcs_sysid==255,"default mode and identities");
        check(c.allowed_commands==std::vector<std::uint16_t>{400,22,21},"command allowlist");
        check(Json::parse(c.resolved_config_json)==base,"resolved config");
        const fs::path output="output/test_results/config_unit";fs::create_directories(output);
        const auto path=output/"case.json";
        auto write=[&](const Json& value){std::ofstream file(path);file<<value.dump(2);};
        auto changed=base;changed["network"]["udp_bind"]["port"]=45000;
        changed["measurement"]["mode"]="B2";changed["measurement"]["duration_seconds"]=0;
        changed["px4"]["expected_sysid"]=23;changed["px4"]["expected_compid"]=42;
        changed["logging"]["root_directory"]="output/custom";write(changed);
        c=mavlink_gateway::load_config(path.string());
        check(c.udp_port==45000&&c.mode=="B2"&&c.measurement_seconds==0&&c.px4_sysid==23&&c.px4_compid==42&&c.output_root=="output/custom","custom values applied");
        std::cout<<"PASS default and custom config\n";
        const std::vector<std::pair<const char*,std::function<void(Json&)>>> cases={
            {"unknown root key",[](Json& j){j["unknown"]=true;}},
            {"missing section",[](Json& j){j.erase("network");}},
            {"unknown nested key",[](Json& j){j["network"]["unknown"]=1;}},
            {"zero port",[](Json& j){j["network"]["udp_bind"]["port"]=0;}},
            {"string port",[](Json& j){j["network"]["udp_bind"]["port"]="14550";}},
            {"negative limit",[](Json& j){j["limits"]["logger_queue_records"]=-1;}},
            {"duplicate role endpoint",[](Json& j){j["network"]["tcp_controller_bind"]=j["network"]["tcp_observer_bind"];}},
            {"multiple controllers",[](Json& j){j["network"]["max_controllers"]=2;}},
            {"zero source ID",[](Json& j){j["px4"]["expected_sysid"]=0;}},
            {"MAVLink v1",[](Json& j){j["protocol"]["mavlink_version"]=1;}},
            {"signed frames",[](Json& j){j["protocol"]["allow_signed_frames"]=true;}},
            {"wrong decoder set",[](Json& j){j["protocol"]["decode_msgids"]={0,1,30,33,76};}},
            {"zero queue",[](Json& j){j["limits"]["client_send_queue_frames"]=0;}},
            {"too many attempts",[](Json& j){j["commands"]["max_total_attempts"]=4;}},
            {"duplicate command",[](Json& j){j["commands"]["allowed_command_ids"]={400,400};}},
            {"progress timeout order",[](Json& j){j["commands"]["in_progress_total_timeout_ms"]=1;}},
            {"unknown mode",[](Json& j){j["measurement"]["mode"]="B3";}},
            {"too small log file",[](Json& j){j["logging"]["max_file_bytes"]=1;}}
        };
        for(const auto& item:cases){auto value=base;item.second(value);write(value);bool rejected=false;
            try{mavlink_gateway::load_config(path.string());}catch(const std::exception&){rejected=true;}
            check(rejected,item.first);std::cout<<"PASS reject "<<item.first<<'\n';}
        {std::ofstream file(path);file<<"{invalid";}
        bool rejected=false;try{mavlink_gateway::load_config(path.string());}catch(const std::exception&){rejected=true;}
        check(rejected,"malformed JSON");
        rejected=false;try{mavlink_gateway::load_config((output/"missing/no_config.json").string());}catch(const std::exception&){rejected=true;}
        check(rejected,"missing file");
        std::cout<<"22/22 config scenarios passed\n";
    }catch(const std::exception& e){std::cerr<<"FAIL: "<<e.what()<<'\n';return 1;}
}
