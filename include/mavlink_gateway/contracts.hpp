#pragma once

// Shared integration contract v1. Implementations belong to A, B and C.
// This header has no dependency on Winsock or the MAVLink C library.
#include <cstddef>
#include <cstdint>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <variant>
#include <vector>

namespace mavlink_gateway {

using Bytes = std::vector<std::uint8_t>;
using SessionId = std::uint64_t;
using SourceId = std::uint64_t;
using FrameId = std::uint64_t;
using MonoNs = std::uint64_t; // Nanoseconds since this process's steady-clock origin.

enum class Transport { Udp, Tcp };
enum class Flow { TelemetryDownlink, ControlUplink };
enum class ClientRole { Observer, Controller };
enum class MeasurementMode { B0, B1, B2 };

struct Endpoint {
    std::string ip;
    std::uint16_t port{}; // Host byte order, never a sockaddr or network-order value.
};

struct SourceRef {
    SourceId source_id{};   // Nonzero, allocated by the network owner in this process.
    SessionId session_id{}; // Zero for UDP; new nonzero ID for every accepted TCP connection.
    Transport transport{Transport::Udp};
    Flow flow{Flow::TelemetryDownlink};
    Endpoint peer;
};

struct ReceiveEvent {
    SourceRef source;
    MonoNs received_at_ns{};
    Bytes bytes; // Owned bytes; must not alias a reusable recv buffer.
};

struct FrameHeader {
    std::uint8_t payload_len{};
    std::uint8_t incompat_flags{};
    std::uint8_t compat_flags{};
    std::uint8_t seq{};
    std::uint8_t sysid{};
    std::uint8_t compid{};
    std::uint32_t msgid{}; // Low 24 bits represent the MAVLink message ID.
};

struct FrameRecord {
    FrameId frame_id{}; // Parser leaves zero; A assigns once before publishing an immutable frame.
    SourceRef source;
    MonoNs received_at_ns{}; // Timestamp of the ReceiveEvent that completed this frame.
    FrameHeader header;
    Bytes raw; // Exact input bytes, including the wire checksum. Never re-encode for forwarding.
};

using SharedFrame = std::shared_ptr<const FrameRecord>;

enum class ParseIssueCode {
    BadCrc,
    UnsupportedMessage,
    UnsupportedSigning,
    UnsupportedIncompatFlags,
    TruncatedDatagram,
    NoiseDiscarded,
    TcpBufferLimit
};

struct ParseIssue {
    SourceRef source;
    MonoNs received_at_ns{};
    ParseIssueCode code{ParseIssueCode::NoiseDiscarded};
    std::size_t discarded_bytes{};
    std::optional<std::uint32_t> msgid;
};

struct ParseBatch {
    std::vector<FrameRecord> frames; // Accepted, unsigned MAVLink 2 frames only, in wire order.
    std::vector<ParseIssue> issues;
    bool close_tcp_session{}; // True only for TCP parser buffer overflow.
};

class IParser {
public:
    virtual ~IParser() = default;
    virtual ParseBatch consume(const ReceiveEvent& event) = 0;
    virtual void reset(SourceId source_id) = 0;
    // Each parser instance is owned by one worker; no concurrent calls on that instance.
    // TCP state is keyed by source_id. No partial UDP frame survives consume().
};

struct Heartbeat {
    std::uint32_t custom_mode{};
    std::uint8_t type{};
    std::uint8_t autopilot{};
    std::uint8_t base_mode{};
    std::uint8_t system_status{};
};

struct SysStatus {
    std::optional<double> battery_voltage_v;
    std::optional<int> battery_remaining_percent;
};

struct Attitude {
    double roll_rad{};
    double pitch_rad{};
    double yaw_rad{};
};

struct GlobalPosition {
    double latitude_deg{};
    double longitude_deg{};
    double altitude_msl_m{};
    double relative_altitude_m{};
};

struct CommandAck {
    std::uint16_t command{};
    std::uint8_t result{};
    std::uint8_t progress{};
    std::int32_t result_param2{};
    std::uint8_t target_system{};
    std::uint8_t target_component{};
};

using DecodedMessage = std::variant<Heartbeat, SysStatus, Attitude, GlobalPosition, CommandAck>;

class IDecoder {
public:
    virtual ~IDecoder() = default;
    // nullopt: valid frame outside the five decoded message types; it remains forwardable.
    virtual std::optional<DecodedMessage> decode(const FrameRecord& frame) const = 0;
};

struct SourceStats {
    SourceId source_id{};
    std::uint8_t sysid{};
    std::uint8_t compid{};
    std::uint64_t received_valid{};
    std::uint64_t estimated_missing{};
    std::uint64_t repeated_seq{};
    std::uint64_t ambiguous_seq{};
    std::optional<MonoNs> last_heartbeat_ns;
    std::map<std::uint32_t, DecodedMessage> latest_messages;
};

struct InspectorSnapshot {
    std::uint64_t valid_frames{};
    std::uint64_t bad_crc_candidates{};
    std::uint64_t unsupported_candidates{};
    std::uint64_t truncated_datagrams{};
    std::uint64_t discarded_noise_bytes{};
    std::uint64_t tcp_buffer_overflows{};
    std::map<std::uint32_t, std::uint64_t> valid_frames_by_msgid;
    std::vector<SourceStats> sources;
};

class IInspector {
public:
    virtual ~IInspector() = default;
    virtual void on_frame(const FrameRecord& frame,
                          const std::optional<DecodedMessage>& decoded) = 0;
    virtual void on_issue(const ParseIssue& issue) = 0;
    virtual InspectorSnapshot snapshot() const = 0;
    // B synchronizes updates/snapshots internally. Socket I/O never runs under its lock.
};

enum class DeliveryStatus {
    SentToSocket,
    ClientQueueFull,
    ConnectionClosed,
    SendError,
    ShutdownDiscard
};

struct DeliveryRecord {
    FrameId frame_id{};
    SessionId client_id{};
    MonoNs event_at_ns{};
    std::optional<MonoNs> send_done_ns; // Set only after all raw bytes entered this socket.
    std::size_t bytes_sent{};
    DeliveryStatus status{DeliveryStatus::SentToSocket};
};

} // namespace mavlink_gateway
