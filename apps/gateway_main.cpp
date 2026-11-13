#include "gateway.hpp"
#include "mavlink_gateway/processing.hpp"
#include <iostream>
#include <map>
#include <set>

namespace {
volatile LONG stop_requested=0;
BOOL WINAPI on_control(DWORD type){
    if(type==CTRL_C_EVENT||type==CTRL_BREAK_EVENT||type==CTRL_CLOSE_EVENT){InterlockedExchange(&stop_requested,1);return TRUE;}
    return FALSE;
}
std::filesystem::path project_root(){
    wchar_t executable[32768]{};
    const auto length=GetModuleFileNameW(nullptr,executable,32768);
    const std::filesystem::path starts[]={length?std::filesystem::path(executable).parent_path():std::filesystem::current_path(),std::filesystem::current_path()};
    for(auto start:starts){
        for(;;){
            if(std::filesystem::exists(start/"include/mavlink_gateway/contracts.hpp")&&std::filesystem::exists(start/"config/gateway.example.json"))return start;
            auto parent=start.parent_path();if(parent==start||parent.empty())break;start=parent;
        }
    }
    return std::filesystem::current_path();
}
int integer(const std::string& text,int minimum,int maximum){
    std::size_t used=0;const auto value=std::stoll(text,&used);
    if(used!=text.size()||value<minimum||value>maximum)throw std::runtime_error("CLI integer out of range: "+text);
    return static_cast<int>(value);
}
}
int main(int argc,char** argv){
    try{
        std::map<std::string,std::string> arguments;
        const std::set<std::string> known{"--config","--run-id","--stop-file","--run-seconds","--send-timeout-ms","--socket-send-buffer-bytes","--send-chunk-bytes"};
        for(int n=1;n<argc;++n){
            const std::string key=argv[n];
            if(key=="--help"){std::cout<<"gateway.exe [--config shared.json] [--run-id ID] [--stop-file PATH] [--run-seconds N]\n";return 0;}
            if(!known.count(key)||n+1>=argc||!arguments.emplace(key,argv[++n]).second)throw std::runtime_error("Unknown, duplicate or incomplete option: "+key);
        }
        const auto root=project_root();
        const auto path=arguments.count("--config")?std::filesystem::path(arguments.at("--config")):root/"config/gateway.example.json";
        auto config=mavlink_gateway::load_config(path.string());
        if(std::filesystem::path(config.output_root).is_relative())config.output_root=(root/config.output_root).lexically_normal().string();
        config.run_id=arguments.count("--run-id")?arguments.at("--run-id"):
            "run_"+std::to_string(std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch()).count())+"_"+std::to_string(GetCurrentProcessId());
        if(config.run_id.empty()||config.run_id.size()>80||config.run_id.find_first_not_of("abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789_-")!=std::string::npos)
            throw std::runtime_error("run-id: at most 80 letters, digits, _ or -");
        if(arguments.count("--stop-file"))config.stop_file=arguments.at("--stop-file");
        if(arguments.count("--run-seconds"))config.run_seconds=integer(arguments.at("--run-seconds"),0,86400);
        if(arguments.count("--send-timeout-ms"))config.send_timeout_ms=integer(arguments.at("--send-timeout-ms"),1,60000);
        if(arguments.count("--socket-send-buffer-bytes"))config.socket_send_buffer_bytes=integer(arguments.at("--socket-send-buffer-bytes"),0,1048576);
        if(arguments.count("--send-chunk-bytes"))config.send_chunk_bytes=integer(arguments.at("--send-chunk-bytes"),0,1048576);
        SetConsoleCtrlHandler(on_control,TRUE);
        auto decoder=std::make_shared<mavlink_gateway::Decoder>();
        auto inspector=std::make_shared<mavlink_gateway::Inspector>();
        std::cout<<"Integrated A+B gateway; MAVLink 2 common parser; mode="<<config.mode<<". Ctrl+C to stop.\n";
        mavlink_gateway::Gateway gateway(config,
            [limit=config.tcp_buffer_bytes]{return std::make_unique<mavlink_gateway::Parser>(limit);},
            decoder,inspector,[inspector](mavlink_gateway::SourceId id){inspector->forget_source(id);});
        gateway.start();
        while(!InterlockedCompareExchange(&stop_requested,0,0)&&!gateway.failed()){
            if(config.run_seconds&&gateway.now_ns()>=std::uint64_t(config.run_seconds)*1000000000ULL)break;
            if(!config.stop_file.empty()&&std::filesystem::exists(config.stop_file))break;
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
        }
        gateway.stop();
        std::cout<<"STOPPED. Results: "<<config.output_root<<'/'<<config.run_id<<"/gateway\n";
        return gateway.failed()?1:0;
    }catch(const std::exception& e){std::cerr<<"ERROR: "<<e.what()<<'\n';return 1;}
}
