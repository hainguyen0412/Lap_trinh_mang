#include "mavlink_gateway/processing.hpp"
#include <common/mavlink.h>
#include <algorithm>
#include <cstring>
#include <limits>
#include <stdexcept>

namespace mavlink_gateway {
namespace {
bool same_source(const SourceRef& a, const SourceRef& b) {
    return a.source_id == b.source_id && a.session_id == b.session_id &&
           a.transport == b.transport && a.flow == b.flow &&
           a.peer.ip == b.peer.ip && a.peer.port == b.peer.port;
}
std::uint32_t message_id(const Bytes& bytes, std::size_t p) {
    return std::uint32_t(bytes[p+7]) | (std::uint32_t(bytes[p+8]) << 8U) |
           (std::uint32_t(bytes[p+9]) << 16U);
}
void issue(ParseBatch& out, const ReceiveEvent& event, ParseIssueCode code,
           std::size_t discarded, std::optional<std::uint32_t> msgid = std::nullopt) {
    out.issues.push_back({event.source, event.received_at_ns, code, discarded, msgid});
}
} // namespace

Parser::Parser(std::size_t limit) : tcp_buffer_limit_(limit) {
    if (limit == 0) throw std::invalid_argument("TCP parser buffer limit must be positive");
}

void Parser::extract(Bytes& bytes, const ReceiveEvent& event, bool datagram, ParseBatch& out) {
    std::size_t p = 0;
    while (p < bytes.size()) {
        const auto begin = bytes.begin() + static_cast<std::ptrdiff_t>(p);
        const auto marker = std::find(begin, bytes.end(), std::uint8_t{0xfd});
        const auto next = static_cast<std::size_t>(marker - bytes.begin());
        if (next > p) issue(out, event, ParseIssueCode::NoiseDiscarded, next-p);
        p = next;
        if (p == bytes.size()) break;
        if (bytes.size()-p < 10) {
            if (datagram) { issue(out,event,ParseIssueCode::TruncatedDatagram,bytes.size()-p); p=bytes.size(); }
            break;
        }
        const auto payload_len = bytes[p+1];
        const auto flags = bytes[p+2];
        const auto msgid = message_id(bytes,p);
        // Unknown incompatible flags have no trusted header layout. Rescan immediately.
        if ((flags & ~std::uint8_t{1}) != 0) {
            issue(out,event,ParseIssueCode::UnsupportedIncompatFlags,1,msgid); ++p; continue;
        }
        const std::size_t length = 12U + payload_len + ((flags & 1U) ? 13U : 0U);
        if (bytes.size()-p < length) {
            if (datagram) { issue(out,event,ParseIssueCode::TruncatedDatagram,bytes.size()-p,msgid); p=bytes.size(); }
            break;
        }
        if ((flags & 1U) != 0) {
            issue(out,event,ParseIssueCode::UnsupportedSigning,length,msgid); p+=length; continue;
        }
        const auto* metadata = mavlink_get_msg_entry(msgid);
        if (!metadata) {
            issue(out,event,ParseIssueCode::UnsupportedMessage,length,msgid); p+=length; continue;
        }
        auto crc = crc_calculate(bytes.data()+p+1, static_cast<std::uint16_t>(9U+payload_len));
        crc_accumulate(metadata->crc_extra,&crc);
        const auto received_crc = std::uint16_t(bytes[p+10U+payload_len]) |
                                  (std::uint16_t(bytes[p+11U+payload_len]) << 8U);
        if (crc != received_crc) {
            issue(out,event,ParseIssueCode::BadCrc,1,msgid); ++p; continue;
        }
        FrameRecord frame;
        frame.source=event.source;
        frame.received_at_ns=event.received_at_ns;
        frame.header={payload_len,flags,bytes[p+3],bytes[p+4],bytes[p+5],bytes[p+6],msgid};
        frame.raw.assign(bytes.begin()+static_cast<std::ptrdiff_t>(p),
                         bytes.begin()+static_cast<std::ptrdiff_t>(p+length));
        out.frames.push_back(std::move(frame));
        p+=length;
    }
    bytes.erase(bytes.begin(),bytes.begin()+static_cast<std::ptrdiff_t>(p));
}

ParseBatch Parser::consume(const ReceiveEvent& event) {
    if (!event.source.source_id) throw std::invalid_argument("source_id must be nonzero");
    ParseBatch out;
    if (event.source.transport == Transport::Udp) {
        if (event.source.session_id || tcp_.count(event.source.source_id))
            throw std::invalid_argument("UDP source must not reuse a TCP source/session");
        auto datagram=event.bytes;
        extract(datagram,event,true,out);
        return out;
    }
    if (!event.source.session_id) throw std::invalid_argument("TCP session_id must be nonzero");
    auto entry=tcp_.try_emplace(event.source.source_id,TcpState{event.source,{}}).first;
    if (!same_source(entry->second.source,event.source))
        throw std::invalid_argument("source_id reused with different connection metadata");
    auto& pending=entry->second.bytes;
    // Parse in bounded chunks, even if the caller supplied a very large recv event.
    for (std::size_t offset=0; offset<event.bytes.size();) {
        const auto amount=std::min<std::size_t>(4096,event.bytes.size()-offset);
        pending.insert(pending.end(),event.bytes.begin()+static_cast<std::ptrdiff_t>(offset),
                        event.bytes.begin()+static_cast<std::ptrdiff_t>(offset+amount));
        offset+=amount;
        extract(pending,event,false,out);
        if (pending.size()>tcp_buffer_limit_) {
            issue(out,event,ParseIssueCode::TcpBufferLimit,pending.size()+event.bytes.size()-offset);
            out.close_tcp_session=true;
            tcp_.erase(entry);
            break;
        }
    }
    return out;
}

void Parser::reset(SourceId source_id) { tcp_.erase(source_id); }

std::optional<DecodedMessage> Decoder::decode(const FrameRecord& frame) const {
    const auto& raw=frame.raw;
    if (raw.size()<12 || raw[0]!=0xfd || raw[2]!=0 || raw.size()!=12U+raw[1] ||
        frame.header.msgid!=message_id(raw,0) || frame.header.payload_len!=raw[1])
        throw std::invalid_argument("Decoder requires a complete accepted unsigned MAVLink 2 frame");
    mavlink_message_t message{}; // Zero-fill trailing payload bytes before generated decoding.
    message.magic=0xfd; message.msgid=frame.header.msgid; message.len=raw[1];
    message.sysid=frame.header.sysid; message.compid=frame.header.compid;
    std::memcpy(_MAV_PAYLOAD_NON_CONST(&message),raw.data()+10,message.len);
    switch (message.msgid) {
    case MAVLINK_MSG_ID_HEARTBEAT: {
        mavlink_heartbeat_t value{}; mavlink_msg_heartbeat_decode(&message,&value);
        return Heartbeat{value.custom_mode,value.type,value.autopilot,value.base_mode,value.system_status};
    }
    case MAVLINK_MSG_ID_SYS_STATUS: {
        mavlink_sys_status_t value{}; mavlink_msg_sys_status_decode(&message,&value);
        SysStatus result;
        if (value.voltage_battery!=std::numeric_limits<std::uint16_t>::max())
            result.battery_voltage_v=value.voltage_battery/1000.0;
        if (value.battery_remaining!=-1) result.battery_remaining_percent=value.battery_remaining;
        return result;
    }
    case MAVLINK_MSG_ID_ATTITUDE: {
        mavlink_attitude_t value{}; mavlink_msg_attitude_decode(&message,&value);
        return Attitude{value.roll,value.pitch,value.yaw};
    }
    case MAVLINK_MSG_ID_GLOBAL_POSITION_INT: {
        mavlink_global_position_int_t value{}; mavlink_msg_global_position_int_decode(&message,&value);
        return GlobalPosition{value.lat/1e7,value.lon/1e7,value.alt/1000.0,value.relative_alt/1000.0};
    }
    case MAVLINK_MSG_ID_COMMAND_ACK: {
        mavlink_command_ack_t value{}; mavlink_msg_command_ack_decode(&message,&value);
        return CommandAck{value.command,value.result,value.progress,value.result_param2,
                          value.target_system,value.target_component};
    }
    default: return std::nullopt;
    }
}

void Inspector::on_frame(const FrameRecord& frame,const std::optional<DecodedMessage>& decoded) {
    std::lock_guard<std::mutex> guard(mutex_);
    ++totals_.valid_frames;
    ++totals_.valid_frames_by_msgid[frame.header.msgid];
    const Key key{frame.source.source_id,frame.header.sysid,frame.header.compid};
    auto& state=sources_[key];
    auto& stats=state.stats;
    stats.source_id=frame.source.source_id; stats.sysid=frame.header.sysid; stats.compid=frame.header.compid;
    ++stats.received_valid;
    if (!state.seq) state.seq=frame.header.seq;
    else {
        const auto d=(int(frame.header.seq)-int(*state.seq)+256)%256;
        if (d==0) ++stats.repeated_seq;
        else if (d<128) { stats.estimated_missing+=static_cast<std::uint64_t>(d-1); state.seq=frame.header.seq; }
        else ++stats.ambiguous_seq; // Keep reference: an old packet must not move it backwards.
    }
    if (frame.header.msgid==MAVLINK_MSG_ID_HEARTBEAT) stats.last_heartbeat_ns=frame.received_at_ns;
    if (decoded) stats.latest_messages.insert_or_assign(frame.header.msgid,*decoded);
}

void Inspector::on_issue(const ParseIssue& issue_value) {
    std::lock_guard<std::mutex> guard(mutex_);
    switch (issue_value.code) {
    case ParseIssueCode::BadCrc: ++totals_.bad_crc_candidates; break;
    case ParseIssueCode::UnsupportedMessage:
    case ParseIssueCode::UnsupportedSigning:
    case ParseIssueCode::UnsupportedIncompatFlags: ++totals_.unsupported_candidates; break;
    case ParseIssueCode::TruncatedDatagram: ++totals_.truncated_datagrams; break;
    case ParseIssueCode::NoiseDiscarded: totals_.discarded_noise_bytes+=issue_value.discarded_bytes; break;
    case ParseIssueCode::TcpBufferLimit: ++totals_.tcp_buffer_overflows; break;
    }
}

InspectorSnapshot Inspector::snapshot() const {
    std::lock_guard<std::mutex> guard(mutex_);
    auto copy=totals_;
    for (const auto& item:sources_) copy.sources.push_back(item.second.stats);
    return copy;
}

void Inspector::forget_source(SourceId source_id) {
    std::lock_guard<std::mutex> guard(mutex_);
    for (auto it=sources_.begin();it!=sources_.end();) {
        if (std::get<0>(it->first)==source_id) it=sources_.erase(it); else ++it;
    }
}

const char* issue_name(ParseIssueCode code) noexcept {
    switch (code) {
    case ParseIssueCode::BadCrc: return "bad_crc";
    case ParseIssueCode::UnsupportedMessage: return "unsupported_message";
    case ParseIssueCode::UnsupportedSigning: return "unsupported_signing";
    case ParseIssueCode::UnsupportedIncompatFlags: return "unsupported_incompat_flags";
    case ParseIssueCode::TruncatedDatagram: return "truncated_datagram";
    case ParseIssueCode::NoiseDiscarded: return "noise_discarded";
    case ParseIssueCode::TcpBufferLimit: return "tcp_buffer_limit";
    }
    return "unknown";
}
const char* delivery_name(DeliveryStatus status) noexcept {
    switch (status) {
    case DeliveryStatus::SentToSocket: return "sent_to_socket";
    case DeliveryStatus::ClientQueueFull: return "client_queue_full";
    case DeliveryStatus::ConnectionClosed: return "connection_closed";
    case DeliveryStatus::SendError: return "send_error";
    case DeliveryStatus::ShutdownDiscard: return "shutdown_discard";
    }
    return "unknown";
}
const char* mode_name(MeasurementMode mode) noexcept {
    switch (mode) { case MeasurementMode::B0:return "B0"; case MeasurementMode::B1:return "B1"; case MeasurementMode::B2:return "B2"; }
    return "unknown";
}
} // namespace mavlink_gateway
