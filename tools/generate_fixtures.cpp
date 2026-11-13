// Reference generator: deliberately does not include/call the project's parser or decoder.
#include <common/mavlink.h>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <sstream>
#include <stdexcept>
#include <vector>

using Bytes=std::vector<std::uint8_t>;
namespace fs=std::filesystem;
template<class Payload,class Encoder>
Bytes encode(const Payload& payload,Encoder encoder,std::uint8_t seq,std::uint8_t sys=1,std::uint8_t comp=1) {
    mavlink_status_t status{}; status.current_tx_seq=seq;
    mavlink_message_t msg{}; encoder(sys,comp,&status,&msg,&payload);
    Bytes raw(MAVLINK_MAX_PACKET_LEN);
    raw.resize(mavlink_msg_to_send_buffer(raw.data(),&msg));
    return raw;
}
std::string hex(const Bytes& raw) {
    std::ostringstream out; out<<std::hex<<std::setfill('0');
    for (auto b:raw) out<<std::setw(2)<<unsigned(b);
    return out.str();
}
void write_binary(const fs::path& path,const Bytes& bytes) {
    std::ofstream out(path,std::ios::binary); out.exceptions(std::ios::failbit|std::ios::badbit);
    out.write(reinterpret_cast<const char*>(bytes.data()),static_cast<std::streamsize>(bytes.size()));
}
int main(int argc,char** argv) {
    try {
        const fs::path dest=argc>1?argv[1]:"tests/fixtures/v1";
        fs::create_directories(dest);
        struct Fixture {std::string name; Bytes raw; std::string expected;};
        std::vector<Fixture> fixtures;
        mavlink_heartbeat_t hb{}; hb.custom_mode=0x12345678; hb.type=2; hb.autopilot=12; hb.base_mode=129; hb.system_status=4;
        fixtures.push_back({"heartbeat",encode(hb,mavlink_msg_heartbeat_encode_status,10),
            R"({"custom_mode":305419896,"type":2,"autopilot":12,"base_mode":129,"system_status":4})"});
        mavlink_sys_status_t sys{}; sys.voltage_battery=12300; sys.battery_remaining=85;
        fixtures.push_back({"sys_status",encode(sys,mavlink_msg_sys_status_encode_status,11),
            R"({"battery_voltage_v":12.3,"battery_remaining_percent":85})"});
        mavlink_attitude_t att{}; att.time_boot_ms=1000; att.roll=0.1f; att.pitch=-0.2f; att.yaw=1.5f;
        fixtures.push_back({"attitude",encode(att,mavlink_msg_attitude_encode_status,12),
            R"({"roll_rad":0.1,"pitch_rad":-0.2,"yaw_rad":1.5})"});
        mavlink_global_position_int_t pos{}; pos.time_boot_ms=1000; pos.lat=210000000; pos.lon=1058000000; pos.alt=123456; pos.relative_alt=5000;
        fixtures.push_back({"global_position_int",encode(pos,mavlink_msg_global_position_int_encode_status,13),
            R"({"latitude_deg":21.0,"longitude_deg":105.8,"altitude_msl_m":123.456,"relative_altitude_m":5.0})"});
        mavlink_command_ack_t ack{}; ack.command=400; ack.result=0; ack.progress=100; ack.target_system=255; ack.target_component=190;
        fixtures.push_back({"command_ack",encode(ack,mavlink_msg_command_ack_encode_status,14),
            R"({"command":400,"result":0,"progress":100,"result_param2":0,"target_system":255,"target_component":190})"});
        mavlink_command_long_t command{}; command.command=400; command.target_system=1; command.target_component=1; command.param1=1;
        fixtures.push_back({"command_long",encode(command,mavlink_msg_command_long_encode_status,0,255,190),"null"});
        mavlink_ping_t ping{}; ping.time_usec=1000000; ping.seq=123;
        fixtures.push_back({"ping",encode(ping,mavlink_msg_ping_encode_status,15),"null"});

        std::ofstream manifest(dest/"manifest.json");
        manifest<<"{\n  \"schema_version\":1,\n  \"reference_commit\":\"41874cfb2af7e658ca9ed88a13e91c750bcd2263\",\n  \"seed\":20261111,\n  \"frames\":[\n";
        Bytes all;
        for (std::size_t i=0;i<fixtures.size();++i) {
            const auto& f=fixtures[i]; write_binary(dest/(f.name+".bin"),f.raw);
            manifest<<"    {\"name\":\""<<f.name<<"\",\"offset\":"<<all.size()<<",\"length\":"<<f.raw.size()
                    <<",\"msgid\":"<<(unsigned(f.raw[7])|(unsigned(f.raw[8])<<8U)|(unsigned(f.raw[9])<<16U))
                    <<",\"seq\":"<<unsigned(f.raw[4])<<",\"sysid\":"<<unsigned(f.raw[5])<<",\"compid\":"<<unsigned(f.raw[6])
                    <<",\"raw_hex\":\""<<hex(f.raw)<<"\",\"expected\":"<<f.expected<<"}"<<(i+1<fixtures.size()?",":"")<<"\n";
            all.insert(all.end(),f.raw.begin(),f.raw.end());
        }
        manifest<<"  ]\n}\n"; manifest.close(); write_binary(dest/"valid.bin",all);
        sys.voltage_battery=65535; sys.battery_remaining=-1;
        write_binary(dest/"battery_unknown.bin",encode(sys,mavlink_msg_sys_status_encode_status,16));
        att={}; write_binary(dest/"attitude_zero_trimmed.bin",encode(att,mavlink_msg_attitude_encode_status,17));
        ack={}; ack.command=400; write_binary(dest/"ack_trimmed.bin",encode(ack,mavlink_msg_command_ack_encode_status,18));

        auto signed_frame=fixtures.front().raw; signed_frame[2]=1;
        auto crc=crc_calculate(signed_frame.data()+1,static_cast<std::uint16_t>(9+signed_frame[1])); crc_accumulate(50,&crc);
        signed_frame[signed_frame.size()-2]=static_cast<std::uint8_t>(crc); signed_frame.back()=static_cast<std::uint8_t>(crc>>8U);
        signed_frame.insert(signed_frame.end(),13,0x42); write_binary(dest/"unsupported_signed.bin",signed_frame);
        auto unknown=fixtures.front().raw; unknown[7]=0xfe; unknown[8]=0xff; unknown[9]=0;
        write_binary(dest/"unsupported_msgid.bin",unknown);
        auto flags=fixtures.front().raw; flags[2]=0x80; write_binary(dest/"unsupported_flags.bin",flags);
        auto bad_crc=fixtures.front().raw; bad_crc.back()^=0x01; write_binary(dest/"bad_crc.bin",bad_crc);
        std::ofstream errors(dest/"errors.json");
        errors<<R"({"cases":[{"file":"bad_crc.bin","issue":"bad_crc"},{"file":"unsupported_signed.bin","issue":"unsupported_signing"},{"file":"unsupported_msgid.bin","issue":"unsupported_message"},{"file":"unsupported_flags.bin","issue":"unsupported_incompat_flags"}]})";
        std::ofstream datagrams(dest/"udp_datagrams.json");
        datagrams<<"{\"datagrams\":[{\"raw_hex\":\""<<hex(all)<<"\",\"accepted_frames\":7},{\"raw_hex\":\""<<hex(Bytes(fixtures.front().raw.begin(),fixtures.front().raw.begin()+8))<<"\",\"accepted_frames\":0,\"issue\":\"truncated_datagram\"}]}\n";
        std::ofstream chunks(dest/"tcp_chunks.json");
        chunks<<R"({"stream":"valid.bin","strategies":["one_byte_per_event","all_bytes_one_event","every_split_of_each_frame","seeded_random_chunks"],"seed":20261111})";
        std::ofstream sequences(dest/"seq_cases.json");
        sequences<<R"({"cases":[{"seq":[254,255,0,1],"gap":0,"repeat":0,"ambiguous":0,"final_reference":1},{"seq":[11,14],"gap":2,"repeat":0,"ambiguous":0,"final_reference":14},{"seq":[20,20,19,21],"gap":0,"repeat":1,"ambiguous":1,"final_reference":21}]})";
        std::cout<<"Reference fixtures: "<<fixtures.size()<<" frames, "<<all.size()<<" bytes; "<<dest.string()<<"\n";
    } catch (const std::exception& ex) {std::cerr<<ex.what()<<'\n';return 1;}
}
