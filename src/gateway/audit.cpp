#include "audit.hpp"
#include <stdexcept>

namespace mavlink_gateway {
std::string csv_row(const std::vector<std::string>& values){
    std::string row;
    for(std::size_t i=0;i<values.size();++i){
        if(i)row+=',';
        row+='"';for(char c:values[i]){if(c=='"')row+='"';row+=c;}row+='"';
    }
    return row+'\n';
}
namespace {
LoggerOptions options_for(const Config& c){
    LoggerOptions options;
    options.root_directory=c.output_root;options.run_id=c.run_id;options.component="gateway";
    options.mode=c.mode=="B0"?MeasurementMode::B0:(c.mode=="B1"?MeasurementMode::B1:MeasurementMode::B2);
    options.seed=c.seed;options.queue_records=c.logger_records;options.max_record_bytes=c.log_record_bytes;
    options.max_file_bytes=c.log_file_bytes;options.max_run_bytes=c.log_run_bytes;
    options.flush_interval_ms=c.log_flush_ms;options.shutdown_drain_ms=c.log_drain_ms;
    options.write_control_files=false;options.reserved_control_bytes=65536;
    return options;
}
}
Audit::Audit(const Config& c):logger_(options_for(c)),directory(logger_.directory()){}
void Audit::log(const std::string& file,std::vector<std::string> values,bool){
    static const std::map<std::string,LogTable> tables{
        {"frames",LogTable::Frames},{"issues",LogTable::Issues},{"delivery",LogTable::Delivery},
        {"events",LogTable::Events},{"metrics",LogTable::Metrics},{"client_summary",LogTable::ClientSummary}};
    const auto it=tables.find(file);
    if(it==tables.end())throw std::invalid_argument("Unknown audit table: "+file);
    logger_.record_row(it->second,values);
}
}
