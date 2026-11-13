#include "mavlink_gateway/processing.hpp"
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <fstream>
#include <filesystem>
#include <functional>
#include <iostream>
#include <iterator>
#include <random>
#include <stdexcept>
#include <thread>

using namespace mavlink_gateway;
namespace fs=std::filesystem;
#define CHECK(condition) do {if(!(condition))throw std::runtime_error(std::string("line ")+std::to_string(__LINE__)+": "+#condition);} while(false)
fs::path fixtures="tests/fixtures/v1";
fs::path results;
Bytes read(const fs::path& path) {
    std::ifstream in(path,std::ios::binary);if(!in)throw std::runtime_error("Cannot read "+path.string());
    return Bytes(std::istreambuf_iterator<char>(in),{});
}
Bytes sample(const std::string& name){return read(fixtures/(name+".bin"));}
Bytes slice(const Bytes& b,std::size_t begin,std::size_t end){return Bytes(b.begin()+static_cast<std::ptrdiff_t>(begin),b.begin()+static_cast<std::ptrdiff_t>(end));}
Bytes concat(Bytes a,const Bytes& b){a.insert(a.end(),b.begin(),b.end());return a;}
ReceiveEvent event(Bytes bytes,Transport transport=Transport::Udp,SourceId source=1,MonoNs ns=100) {
    return {{source,transport==Transport::Tcp?source:0,transport,Flow::TelemetryDownlink,{"127.0.0.1",14580}},ns,std::move(bytes)};
}
bool has(const ParseBatch& batch,ParseIssueCode code) {
    return std::any_of(batch.issues.begin(),batch.issues.end(),[code](const auto& i){return i.code==code;});
}
bool near(double a,double b){return std::abs(a-b)<1e-6;}
FrameRecord one(const std::string& name){Parser parser;auto batch=parser.consume(event(sample(name)));CHECK(batch.frames.size()==1);return batch.frames.front();}
void throws(const std::function<void()>& operation){bool caught=false;try{operation();}catch(const std::exception&){caught=true;}CHECK(caught);}
void u01(){
    Parser parser;Decoder decoder;const auto raw=sample("valid");auto batch=parser.consume(event(raw));
    CHECK(batch.frames.size()==7);CHECK(batch.issues.empty());Bytes joined;
    const std::vector<unsigned> ids{0,1,30,33,77,76,4};
    for(std::size_t i=0;i<batch.frames.size();++i){const auto& f=batch.frames[i];CHECK(f.header.msgid==ids[i]);CHECK(f.frame_id==0);joined=concat(std::move(joined),f.raw);}
    CHECK(joined==raw);
    auto hb=std::get<Heartbeat>(*decoder.decode(batch.frames[0]));CHECK(hb.custom_mode==0x12345678);CHECK(hb.autopilot==12);CHECK(hb.base_mode==129);CHECK(hb.system_status==4);
    auto sys=std::get<SysStatus>(*decoder.decode(batch.frames[1]));CHECK(near(*sys.battery_voltage_v,12.3));CHECK(*sys.battery_remaining_percent==85);
    auto att=std::get<Attitude>(*decoder.decode(batch.frames[2]));CHECK(near(att.roll_rad,.1));CHECK(near(att.pitch_rad,-.2));CHECK(near(att.yaw_rad,1.5));
    auto pos=std::get<GlobalPosition>(*decoder.decode(batch.frames[3]));CHECK(near(pos.latitude_deg,21));CHECK(near(pos.longitude_deg,105.8));CHECK(near(pos.altitude_msl_m,123.456));CHECK(near(pos.relative_altitude_m,5));
    auto ack=std::get<CommandAck>(*decoder.decode(batch.frames[4]));CHECK(ack.command==400);CHECK(ack.result==0);CHECK(ack.progress==100);CHECK(ack.target_system==255);CHECK(ack.target_component==190);
    CHECK(!decoder.decode(batch.frames[5]));CHECK(!decoder.decode(batch.frames[6]));
}
void u02(){
    for(const auto& name:{"heartbeat","sys_status","attitude","global_position_int","command_ack","command_long","ping"}){
        const auto raw=sample(name);
        for(std::size_t split=0;split<=raw.size();++split){
            Parser parser;auto first=parser.consume(event(slice(raw,0,split),Transport::Tcp,1,10));
            auto last=parser.consume(event(slice(raw,split,raw.size()),Transport::Tcp,1,20));
            CHECK(first.issues.empty());CHECK(last.issues.empty());CHECK(first.frames.size()+last.frames.size()==1);
            const auto& f=first.frames.empty()?last.frames.front():first.frames.front();CHECK(f.raw==raw);CHECK(f.received_at_ns==(split==raw.size()?10U:20U));
        }
    }
}
void u03(){
    const auto raw=sample("valid");std::mt19937 random(20261111);
    for(unsigned run=0;run<32;++run){Parser parser;Bytes joined;std::size_t count=0;
        for(std::size_t p=0;p<raw.size();){const auto end=std::min(raw.size(),p+(run==0?1:1+random()%45));auto b=parser.consume(event(slice(raw,p,end),Transport::Tcp));
            CHECK(b.issues.empty());for(const auto& f:b.frames){joined=concat(std::move(joined),f.raw);++count;}p=end;}
        CHECK(joined==raw);CHECK(count==7);
    }
}
void u04(){
    Parser p;const auto a=sample("heartbeat"),b=sample("attitude");
    CHECK(p.consume(event(slice(a,0,7),Transport::Tcp,1)).frames.empty());
    CHECK(p.consume(event(slice(b,0,13),Transport::Tcp,2)).frames.empty());
    auto aa=p.consume(event(slice(a,7,a.size()),Transport::Tcp,1));CHECK(aa.frames.size()==1);CHECK(aa.frames[0].raw==a);
    auto bb=p.consume(event(slice(b,13,b.size()),Transport::Tcp,2));CHECK(bb.frames.size()==1);CHECK(bb.frames[0].raw==b);
}
void u05(){
    for(bool payload:{false,true}){auto broken=sample("heartbeat");broken[payload?10:broken.size()-1]^=0x01;
        Parser p;auto result=p.consume(event(concat(broken,sample("sys_status"))));CHECK(result.frames.size()==1);CHECK(result.frames[0].header.msgid==1);CHECK(has(result,ParseIssueCode::BadCrc));}
}
void u06(){
    Decoder d;auto battery=std::get<SysStatus>(*d.decode(one("battery_unknown")));CHECK(!battery.battery_voltage_v);CHECK(!battery.battery_remaining_percent);
    auto frame=one("attitude_zero_trimmed");CHECK(frame.header.payload_len==1);auto attitude=std::get<Attitude>(*d.decode(frame));CHECK(attitude.roll_rad==0);CHECK(attitude.pitch_rad==0);CHECK(attitude.yaw_rad==0);
    auto ack_frame=one("ack_trimmed");CHECK(ack_frame.header.payload_len<3);auto ack=std::get<CommandAck>(*d.decode(ack_frame));CHECK(ack.command==400);CHECK(ack.result==0);CHECK(ack.target_system==0);CHECK(ack.target_component==0);
}
void u07(){Decoder d;auto ping=one("ping");CHECK(!d.decode(ping));Inspector i;i.on_frame(ping,std::nullopt);auto s=i.snapshot();CHECK(s.valid_frames==1);CHECK(s.valid_frames_by_msgid.at(4)==1);CHECK(s.sources[0].received_valid==1);}
void u08(){
    for(const auto& item:std::vector<std::pair<std::string,ParseIssueCode>>{{"unsupported_signed",ParseIssueCode::UnsupportedSigning},{"unsupported_msgid",ParseIssueCode::UnsupportedMessage},{"unsupported_flags",ParseIssueCode::UnsupportedIncompatFlags}}){
        Parser p;auto result=p.consume(event(concat(sample(item.first),sample("heartbeat"))));CHECK(result.frames.size()==1);CHECK(result.frames[0].raw==sample("heartbeat"));CHECK(has(result,item.second));CHECK(!has(result,ParseIssueCode::BadCrc));}
}
void u09(){Parser p;Bytes bytes{0xaa,0x12,0x00,0xfe};bytes=concat(std::move(bytes),sample("bad_crc"));bytes=concat(std::move(bytes),sample("heartbeat"));auto b=p.consume(event(bytes));CHECK(b.frames.size()==1);CHECK(has(b,ParseIssueCode::NoiseDiscarded));CHECK(has(b,ParseIssueCode::BadCrc));}
void u10(){
    Parser p;auto hb=sample("heartbeat");auto b=p.consume(event(concat(sample("valid"),slice(hb,0,8))));CHECK(b.frames.size()==7);CHECK(has(b,ParseIssueCode::TruncatedDatagram));
    CHECK(p.consume(event(slice(hb,8,hb.size()))).frames.empty());CHECK(p.consume(event(hb)).frames.size()==1);
}
FrameRecord seq_frame(unsigned seq,SourceId source=1,std::uint8_t sysid=1){FrameRecord f;f.source=event({}).source;f.source.source_id=source;f.header.sysid=sysid;f.header.compid=1;f.header.seq=static_cast<std::uint8_t>(seq);f.header.msgid=0;f.received_at_ns=seq+1;return f;}
void u11(){
    Inspector wrap;for(unsigned n:{254,255,0,1})wrap.on_frame(seq_frame(n),std::nullopt);auto s=wrap.snapshot();CHECK(s.sources[0].estimated_missing==0);CHECK(s.sources[0].received_valid==4);
    Inspector i;for(unsigned n:{20,20,19,21})i.on_frame(seq_frame(n),std::nullopt);i.on_frame(seq_frame(11,2),std::nullopt);i.on_frame(seq_frame(14,2),std::nullopt);i.on_frame(seq_frame(99,2,2),std::nullopt);
    s=i.snapshot();CHECK(s.sources.size()==3);CHECK(s.sources[0].repeated_seq==1);CHECK(s.sources[0].ambiguous_seq==1);CHECK(s.sources[0].estimated_missing==0);CHECK(s.sources[1].estimated_missing==2);CHECK(s.sources[2].estimated_missing==0);
    CHECK(s.sources[0].last_heartbeat_ns==22);i.forget_source(1);CHECK(i.snapshot().sources.size()==2);CHECK(i.snapshot().valid_frames==7);
}
void buffer_limit(){
    Parser p(16);auto hb=sample("heartbeat");auto b=p.consume(event(slice(hb,0,hb.size()-1),Transport::Tcp));CHECK(b.close_tcp_session);CHECK(has(b,ParseIssueCode::TcpBufferLimit));
    p.reset(1);CHECK(p.consume(event(hb,Transport::Tcp,2)).frames.size()==1);
    Parser normal;Bytes noise(100000,0x55);auto result=normal.consume(event(concat(noise,hb),Transport::Tcp));CHECK(!result.close_tcp_session);CHECK(result.frames.size()==1);
}
void reset_session(){
    Parser p;auto hb=sample("heartbeat");p.consume(event(slice(hb,0,8),Transport::Tcp));p.reset(1);
    CHECK(p.consume(event(slice(hb,8,hb.size()),Transport::Tcp,2)).frames.empty());CHECK(p.consume(event(hb,Transport::Tcp,2)).frames.size()==1);
    auto changed=event(hb,Transport::Tcp,2);changed.source.peer.port=1234;throws([&]{p.consume(changed);});
    changed=event(hb);changed.source.source_id=0;throws([&]{p.consume(changed);});
}
void concurrent_snapshot(){
    Inspector inspector;std::vector<std::thread> workers;std::atomic<bool> done{false};
    std::thread reader([&]{while(!done.load())(void)inspector.snapshot();});
    for(unsigned t=1;t<=4;++t)workers.emplace_back([&,t]{for(unsigned n=0;n<500;++n)inspector.on_frame(seq_frame(n,t),std::nullopt);});
    for(auto& worker:workers){worker.join();}
    done=true;reader.join();auto s=inspector.snapshot();CHECK(s.valid_frames==2000);CHECK(s.sources.size()==4);
    for(const auto& source:s.sources){CHECK(source.received_valid==500);CHECK(source.estimated_missing==0);}
}
int main(){try{u01();std::cout<<"PASS u01\n";u02();std::cout<<"PASS u02\n";u03();std::cout<<"PASS u03\n";u04();std::cout<<"PASS u04\n";u05();std::cout<<"PASS u05\n";u06();std::cout<<"PASS u06\n";u07();std::cout<<"PASS u07\n";u08();std::cout<<"PASS u08\n";u09();std::cout<<"PASS u09\n";u10();std::cout<<"PASS u10\n";u11();std::cout<<"PASS u11\n";buffer_limit();std::cout<<"PASS buffer_limit\n";reset_session();std::cout<<"PASS reset_session\n";concurrent_snapshot();std::cout<<"PASS concurrent_snapshot\n";std::cout<<"14/14 processing groups passed\n";}catch(const std::exception& e){std::cerr<<e.what();return 1;}}
