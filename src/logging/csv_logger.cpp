#include "mavlink_gateway/csv_logger.hpp"
#include <algorithm>
#include <array>
#include <chrono>
#include <condition_variable>
#include <deque>
#include <fstream>
#include <iomanip>
#include <sstream>
#include <stdexcept>
#include <thread>
#include <ctime>

namespace mavlink_gateway {
namespace {
using Clock=std::chrono::steady_clock;
const std::array<std::vector<std::string>,7> headers{{
    {"run_id","component","frame_id","source_id","session_id","rx_ns","sysid","compid","msgid","seq","payload_len","raw_len"},
    {"run_id","component","source_id","session_id","event_ns","code","discarded_bytes","msgid"},
    {"run_id","component","frame_id","client_id","event_ns","send_done_ns","bytes_sent","status"},
    {"run_id","component","request_id","session_id","command","target_system","target_component","attempt","event_ns","event","result","progress"},
    {"run_id","component","event_ns","session_id","source_id","event","detail"},
    {"run_id","component","sample_ns","valid_total","bad_crc_total","unsupported_total","truncated_datagrams","noise_bytes","seq_gap_total","seq_repeat_total","seq_ambiguous_total","udp_queue_drop","client_queue_drop","log_drop","active_clients","rx_frames_per_s","tx_bytes_per_s","cpu_percent","rss_bytes","udp_queue_depth","max_client_queue_frames"},
    {"run_id","component","client_id","role","attempted_window","sent_window","not_sent_window","bytes_sent_window","latency_samples","latency_p95_ms","latency_max_ms","latency_sampling_truncated"}
}};
const std::array<const char*,7> filenames{{"frames.csv","issues.csv","delivery.csv","commands.csv","events.csv","metrics.csv","client_summary.csv"}};
const std::vector<std::string> summary_header{"run_id","component","mode","clients","measurement_s","valid_window","sent_window","not_sent_window","pending_after_drain","latency_samples","latency_p50_ms","latency_p95_ms","latency_max_ms","log_drop"};
std::string csv(const std::vector<std::string>& values) {
    std::string line;
    for(std::size_t i=0;i<values.size();++i) {
        if(i) line+=',';
        if(values[i].find_first_of(",\"\r\n")==std::string::npos) line+=values[i];
        else { line+='"'; for(char c:values[i]) {if(c=='"') line+='"';line+=c;} line+='"'; }
    }
    return line+'\n';
}
std::string json_string(const std::string& text) {
    std::ostringstream out; out<<'"';
    for(unsigned char c:text) {
        if(c=='"'||c=='\\') out<<'\\'<<char(c);
        else if(c<32) out<<"\\u"<<std::hex<<std::setw(4)<<std::setfill('0')<<unsigned(c)<<std::dec;
        else out<<char(c);
    }
    out<<'"';return out.str();
}
std::string utc_now() {
    static std::mutex mutex; std::lock_guard<std::mutex> lock(mutex);
    const auto t=std::chrono::system_clock::to_time_t(std::chrono::system_clock::now());
    std::ostringstream out;out<<std::put_time(std::gmtime(&t),"%Y-%m-%dT%H:%M:%SZ");return out.str();
}
bool safe_id(const std::string& id) {
    if(id.empty()||id.size()>80) return false;
    for(unsigned char c:id) if(!((c>='a'&&c<='z')||(c>='A'&&c<='Z')||(c>='0'&&c<='9')||c=='_'||c=='-')) return false;
    return true;
}
std::vector<std::string> assemble(const std::vector<std::string>& columns,
                                 const std::map<std::string,std::string>& fields) {
    std::vector<std::string> values(columns.size());
    for(const auto& item:fields) {
        auto it=std::find(columns.begin(),columns.end(),item.first);
        if(it==columns.end()||item.first=="run_id"||item.first=="component")
            throw std::invalid_argument("Unknown/reserved CSV column: "+item.first);
        values[static_cast<std::size_t>(it-columns.begin())]=item.second;
    }
    return values;
}
} // namespace

struct CsvLogger::Impl {
    LoggerOptions options;
    std::filesystem::path dir;
    std::string started{utc_now()};
    struct Entry{std::size_t table;std::string line;};
    mutable std::mutex mutex;
    std::condition_variable wake;
    std::deque<Entry> pending;
    LoggerStats statistics;
    std::array<std::ofstream,7> streams;
    std::array<std::uint64_t,7> sizes{};
    std::uint64_t csv_bytes{};
    bool stopping{};
    bool capped{};
    Clock::time_point deadline;
    std::thread worker;
    std::once_flag close_once;
    std::vector<std::string> summary=std::vector<std::string>(summary_header.size());

    explicit Impl(LoggerOptions opts):options(std::move(opts)) {
        if(!safe_id(options.run_id)||!safe_id(options.component)||!options.queue_records||
           !options.max_record_bytes||!options.flush_interval_ms||options.max_file_bytes<1024||
           options.reserved_control_bytes<4096||options.reserved_control_bytes>options.max_run_bytes||
           options.max_run_bytes-options.reserved_control_bytes<4096)
            throw std::invalid_argument("Invalid logger IDs/limits");
        dir=options.root_directory/options.run_id/options.component;
        if(std::filesystem::exists(dir)&&!std::filesystem::is_empty(dir))
            throw std::invalid_argument("Refusing to overwrite a nonempty log directory: "+dir.string());
        std::filesystem::create_directories(dir);
        if(options.write_control_files)metadata();
        worker=std::thread([this]{run();});
    }
    void metadata() {
        const auto s=get_stats();
        std::ostringstream text;
        text<<"{\n\"schema_version\":1,\"run_id\":"<<json_string(options.run_id)
            <<",\"component\":"<<json_string(options.component)<<",\"started_at_iso8601\":"<<json_string(started)
            <<",\"scope\":"<<json_string(options.scope)<<",\"mavlink_commit\":\"41874cfb2af7e658ca9ed88a13e91c750bcd2263\""
            <<",\"seed\":"<<options.seed<<",\"resolved_config\":{\"mode\":"<<json_string(mode_name(options.mode))
            <<",\"logger_queue_records\":"<<options.queue_records<<",\"max_file_bytes\":"<<options.max_file_bytes
            <<",\"max_component_run_bytes\":"<<options.max_run_bytes<<"},\"metadata\":{";
        bool first=true;
        for(const auto& item:options.metadata){if(!first)text<<',';first=false;text<<json_string(item.first)<<':'<<json_string(item.second);}
        text<<"},\"log_drop\":"<<s.dropped<<",\"log_truncated\":"<<(s.log_truncated?"true":"false")
            <<",\"last_error\":"<<json_string(s.last_error)<<"}\n";
        if(text.str().size()>std::min<std::uint64_t>(2048,options.max_file_bytes))
            throw std::invalid_argument("Run metadata exceeds reserved/file budget");
        std::ofstream file(dir/"run.json",std::ios::binary|std::ios::trunc);
        file.exceptions(std::ios::failbit|std::ios::badbit);file<<text.str();file.close();
    }
    LoggerStats get_stats() const {std::lock_guard<std::mutex> lock(mutex);return statistics;}
    void enqueue(std::size_t table,std::string line) {
        std::lock_guard<std::mutex> lock(mutex);
        if(table<3&&options.mode!=MeasurementMode::B2){++statistics.disabled;return;}
        ++statistics.submitted;
        if(stopping||pending.size()>=options.queue_records||line.size()>options.max_record_bytes){++statistics.dropped;return;}
        pending.push_back({table,std::move(line)});wake.notify_one();
    }
    void write(const Entry& e) {
        const auto head=sizes[e.table]?std::string{}:csv(headers[e.table]);
        const auto size=head.size()+e.line.size();
        if(capped||sizes[e.table]+size>options.max_file_bytes||csv_bytes+size>options.max_run_bytes-options.reserved_control_bytes) {
            capped=true;std::lock_guard<std::mutex> lock(mutex);
            ++statistics.dropped;statistics.log_truncated=true;return;
        }
        auto& file=streams[e.table];
        if(!file.is_open()) {
            file.open(dir/filenames[e.table],std::ios::binary|std::ios::trunc);
            file.exceptions(std::ios::failbit|std::ios::badbit);
        }
        file<<head<<e.line;sizes[e.table]+=size;csv_bytes+=size;
        std::lock_guard<std::mutex> lock(mutex);++statistics.written;
    }
    void flush() noexcept {
        for(auto& file:streams) if(file.is_open()) {
            try{file.flush();}catch(const std::exception& e){
                std::lock_guard<std::mutex> lock(mutex);statistics.last_error=e.what();statistics.log_truncated=true;capped=true;
            }
        }
    }
    void run() noexcept {
        auto next_flush=Clock::now()+std::chrono::milliseconds(options.flush_interval_ms);
        for(;;) {
            std::optional<Entry> entry;
            {
                std::unique_lock<std::mutex> lock(mutex);
                wake.wait_until(lock,next_flush,[this]{return stopping||!pending.empty();});
                if(stopping&&Clock::now()>deadline){statistics.dropped+=pending.size();pending.clear();}
                if(pending.empty()&&stopping)break;
                if(!pending.empty()){entry=std::move(pending.front());pending.pop_front();}
            }
            if(entry){
                try{write(*entry);}catch(const std::exception& e){
                    std::lock_guard<std::mutex> lock(mutex);++statistics.dropped;statistics.last_error=e.what();statistics.log_truncated=true;capped=true;
                }
            }
            if(Clock::now()>=next_flush){flush();next_flush=Clock::now()+std::chrono::milliseconds(options.flush_interval_ms);}
        }
        flush();
        for(auto& file:streams) if(file.is_open()) {
            try{file.flush();file.close();}catch(const std::exception& e){std::lock_guard<std::mutex> lock(mutex);statistics.last_error=e.what();}
        }
    }
    void close() {
        std::call_once(close_once,[this]{
            {std::lock_guard<std::mutex> lock(mutex);stopping=true;deadline=Clock::now()+std::chrono::milliseconds(options.shutdown_drain_ms);}
            wake.notify_all();if(worker.joinable())worker.join();
            if(!options.write_control_files)return;
            try {
                summary[0]=options.run_id;summary[1]=options.component;summary[2]=mode_name(options.mode);
                summary.back()=std::to_string(get_stats().dropped);
                const auto text=csv(summary_header)+csv(summary);
                if(text.size()>std::min<std::uint64_t>(2048,options.max_file_bytes))throw std::invalid_argument("Summary exceeds reserved/file budget");
                std::ofstream file(dir/"summary.csv",std::ios::binary|std::ios::trunc);
                file.exceptions(std::ios::failbit|std::ios::badbit);file<<text;file.close();metadata();
            } catch(const std::exception& e){std::lock_guard<std::mutex> lock(mutex);statistics.last_error=e.what();}
        });
    }
    ~Impl(){close();}
};

CsvLogger::CsvLogger(LoggerOptions options):impl_(std::make_unique<Impl>(std::move(options))){}
CsvLogger::~CsvLogger()=default;
void CsvLogger::record_fields(LogTable table,const std::map<std::string,std::string>& fields) {
    const auto index=static_cast<std::size_t>(table);
    if(index>=headers.size())throw std::invalid_argument("Unknown CSV table");
    auto row=assemble(headers[index],fields);row[0]=impl_->options.run_id;row[1]=impl_->options.component;
    impl_->enqueue(index,csv(row));
}
void CsvLogger::record_row(LogTable table,const std::vector<std::string>& fields) {
    const auto index=static_cast<std::size_t>(table);
    if(index>=headers.size()||fields.size()+2!=headers[index].size())
        throw std::invalid_argument("CSV row does not match table schema");
    std::vector<std::string> row{impl_->options.run_id,impl_->options.component};
    row.insert(row.end(),fields.begin(),fields.end());
    impl_->enqueue(index,csv(row));
}
void CsvLogger::record_frame(const FrameRecord& f) {
    record_fields(LogTable::Frames,{{"frame_id",std::to_string(f.frame_id)},{"source_id",std::to_string(f.source.source_id)},
        {"session_id",std::to_string(f.source.session_id)},{"rx_ns",std::to_string(f.received_at_ns)},
        {"sysid",std::to_string(f.header.sysid)},{"compid",std::to_string(f.header.compid)},
        {"msgid",std::to_string(f.header.msgid)},{"seq",std::to_string(f.header.seq)},
        {"payload_len",std::to_string(f.header.payload_len)},{"raw_len",std::to_string(f.raw.size())}});
}
void CsvLogger::record_issue(const ParseIssue& i) {
    record_fields(LogTable::Issues,{{"source_id",std::to_string(i.source.source_id)},{"session_id",std::to_string(i.source.session_id)},
        {"event_ns",std::to_string(i.received_at_ns)},{"code",issue_name(i.code)},
        {"discarded_bytes",std::to_string(i.discarded_bytes)},{"msgid",i.msgid?std::to_string(*i.msgid):""}});
}
void CsvLogger::record_delivery(const DeliveryRecord& d) {
    record_fields(LogTable::Delivery,{{"frame_id",std::to_string(d.frame_id)},{"client_id",std::to_string(d.client_id)},
        {"event_ns",std::to_string(d.event_at_ns)},{"send_done_ns",d.send_done_ns?std::to_string(*d.send_done_ns):""},
        {"bytes_sent",std::to_string(d.bytes_sent)},{"status",delivery_name(d.status)}});
}
void CsvLogger::set_summary(const std::map<std::string,std::string>& fields) {
    if(fields.count("mode")||fields.count("log_drop"))throw std::invalid_argument("Logger owns mode and log_drop");
    auto row=assemble(summary_header,fields);
    std::lock_guard<std::mutex> lock(impl_->mutex);
    if(impl_->stopping)throw std::logic_error("Logger already closed");
    impl_->summary=std::move(row);
}
LoggerStats CsvLogger::stats() const{return impl_->get_stats();}
void CsvLogger::close(){impl_->close();}
std::filesystem::path CsvLogger::directory() const{return impl_->dir;}
} // namespace mavlink_gateway
