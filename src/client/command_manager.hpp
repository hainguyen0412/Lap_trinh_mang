#pragma once
// Command Manager: MOT yeu cau lenh dang cho tai mot thoi diem.
//  - gui lan dau confirmation=0, cho ACK 1 giay
//  - chua co ACK: gui lai, TONG CONG toi da 3 lan gui (confirmation tang theo lan gui)
//  - ACK IN_PROGRESS: khong gui lai; cho toi da 10 s khong cap nhat, tong toi da 60 s
//  - mat ket noi khi dang cho: ket qua Unknown, KHONG tu phat lai
// Lop nay khong co socket, khong co thread, khong goi sleep: thoi gian duoc truyen vao
// (tham so `now`), nen test duoc bang dong ho gia. KHONG tu khoa: noi goi tu giu mutex.
#include "mavlink_gateway/contracts.hpp"
#include <array>
#include <cstdint>
#include <functional>
#include <optional>
#include <string>

namespace cclient {
using mavlink_gateway::Bytes;
using mavlink_gateway::CommandAck;
using mavlink_gateway::MonoNs;

constexpr std::uint16_t kCmdArm = 400;     // MAV_CMD_COMPONENT_ARM_DISARM
constexpr std::uint16_t kCmdTakeoff = 22;  // MAV_CMD_NAV_TAKEOFF
constexpr std::uint16_t kCmdLand = 21;     // MAV_CMD_NAV_LAND

using Params = std::array<float, 7>;

enum class Outcome { None, Accepted, Rejected, Timeout, Unknown, SendFailed };
const char* outcome_name(Outcome o);

struct CommandEvent {
    std::uint64_t request_id = 0;
    std::uint16_t command = 0;
    std::uint8_t target_system = 0;
    std::uint8_t target_component = 0;
    unsigned attempt = 0;
    MonoNs at_ns = 0;
    // submitted, queued, ack_in_progress, ack_accepted, ack_rejected, ack_ignored, ack_late,
    // final_timeout, final_unknown, send_failed, rejected_busy, rejected_disconnected
    std::string event;
    std::optional<int> result;
    std::optional<int> progress;
};

struct CommandConfig {
    std::uint8_t px4_sysid = 1, px4_compid = 1;
    std::uint8_t gcs_sysid = 255, gcs_compid = 190;
    MonoNs ack_timeout_ns = 1000000000ULL;
    unsigned max_sends = 3;
    MonoNs progress_idle_ns = 10000000000ULL;
    MonoNs progress_total_ns = 60000000000ULL;
};

class CommandManager {
public:
    using BuildFn = std::function<Bytes(std::uint16_t command, const Params&, std::uint8_t confirmation)>;
    using SendFn = std::function<bool(const Bytes&)>;   // true = da dua vao hang doi gui
    using EventFn = std::function<void(const CommandEvent&)>;

    CommandManager(CommandConfig cfg, BuildFn build, SendFn send, EventFn on_event);

    // Tra ve request_id (>0), hoac 0 neu dang ban/da mat ket noi/gui loi.
    std::uint64_t submit(std::uint16_t command, const Params& params, MonoNs now);
    // src_sys/src_comp: sysid/compid trong header khung COMMAND_ACK.
    void on_ack(const CommandAck& ack, std::uint8_t src_sys, std::uint8_t src_comp, MonoNs now);
    void tick(MonoNs now);
    void on_disconnect(MonoNs now);

    bool busy() const { return state_ != State::Idle; }
    Outcome last_outcome() const { return last_outcome_; }

private:
    enum class State { Idle, WaitingAck, InProgress };
    struct Pending {
        std::uint64_t request_id = 0;
        std::uint16_t command = 0;
        Params params{};
        unsigned attempts = 0;
        MonoNs last_sent_ns = 0, first_progress_ns = 0, last_progress_ns = 0;
    };
    bool send_attempt(MonoNs now);
    void finish(Outcome outcome, const char* event, MonoNs now,
                std::optional<int> result = std::nullopt, std::optional<int> progress = std::nullopt);
    void emit(std::uint64_t id, std::uint16_t command, unsigned attempt, const char* event, MonoNs now,
              std::optional<int> result = std::nullopt, std::optional<int> progress = std::nullopt);

    CommandConfig cfg_;
    BuildFn build_;
    SendFn send_;
    EventFn on_event_;
    State state_ = State::Idle;
    Pending pending_;
    std::uint64_t next_id_ = 1;
    bool disconnected_ = false;
    Outcome last_outcome_ = Outcome::None;
    std::uint64_t last_request_id_ = 0;
    std::uint16_t last_command_ = 0;
};
} // namespace cclient
