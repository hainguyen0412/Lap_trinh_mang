#pragma once
#include "mavlink_gateway/config.hpp"
#include "mavlink_gateway/csv_logger.hpp"

namespace mavlink_gateway {
std::string csv_row(const std::vector<std::string>& values);
// A supplies network records; B owns queued CSV I/O.
class Audit {
    CsvLogger logger_;
public:
    std::filesystem::path directory;
    explicit Audit(const Config&);
    void log(const std::string& file,std::vector<std::string> values,bool detail=false);
    void close(){logger_.close();}
    std::uint64_t dropped() const{return logger_.stats().dropped;}
    bool truncated() const{return logger_.stats().log_truncated;}
    bool failed() const{return !logger_.stats().last_error.empty();}
};
}
