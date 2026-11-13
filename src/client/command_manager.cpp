#include "command_manager.hpp"

namespace cclient {
namespace {
constexpr int kResultAccepted = 0;   // MAV_RESULT_ACCEPTED
constexpr int kResultInProgress = 5; // MAV_RESULT_IN_PROGRESS
} // namespace

const char* outcome_name(Outcome o) {
    switch (o) {
    case Outcome::None: return "none";
    case Outcome::Accepted: return "accepted";
    case Outcome::Rejected: return "rejected";
    case Outcome::Timeout: return "timeout";
    case Outcome::Unknown: return "unknown";
    case Outcome::SendFailed: return "send_failed";
    }
    return "unknown";
}

CommandManager::CommandManager(CommandConfig cfg, BuildFn build, SendFn send, EventFn on_event)
    : cfg_(cfg), build_(std::move(build)), send_(std::move(send)), on_event_(std::move(on_event)) {}

void CommandManager::emit(std::uint64_t id, std::uint16_t command, unsigned attempt, const char* event,
                          MonoNs now, std::optional<int> result, std::optional<int> progress) {
    if (!on_event_) return;
    CommandEvent ev;
    ev.request_id = id;
    ev.command = command;
    ev.target_system = cfg_.px4_sysid;
    ev.target_component = cfg_.px4_compid;
    ev.attempt = attempt;
    ev.at_ns = now;
    ev.event = event;
    ev.result = result;
    ev.progress = progress;
    on_event_(ev);
}

void CommandManager::finish(Outcome outcome, const char* event, MonoNs now, std::optional<int> result,
                            std::optional<int> progress) {
    emit(pending_.request_id, pending_.command, pending_.attempts, event, now, result, progress);
    last_outcome_ = outcome;
    last_request_id_ = pending_.request_id;
    last_command_ = pending_.command;
    state_ = State::Idle;
}

bool CommandManager::send_attempt(MonoNs now) {
    ++pending_.attempts;
    pending_.last_sent_ns = now;
    const auto confirmation = static_cast<std::uint8_t>(pending_.attempts - 1);
    Bytes raw;
    try {
        raw = build_(pending_.command, pending_.params, confirmation);
    } catch (...) {
        finish(Outcome::SendFailed, "send_failed", now);
        return false;
    }
    if (!send_(raw)) {
        finish(Outcome::SendFailed, "send_failed", now);
        return false;
    }
    emit(pending_.request_id, pending_.command, pending_.attempts, "queued", now);
    return true;
}

std::uint64_t CommandManager::submit(std::uint16_t command, const Params& params, MonoNs now) {
    if (disconnected_) {
        emit(0, command, 0, "rejected_disconnected", now);
        return 0;
    }
    if (state_ != State::Idle) {
        emit(pending_.request_id, command, 0, "rejected_busy", now);
        return 0;
    }
    pending_ = Pending{};
    pending_.request_id = next_id_++;
    pending_.command = command;
    pending_.params = params;
    const auto id = pending_.request_id;
    last_outcome_ = Outcome::None;
    state_ = State::WaitingAck;
    emit(id, command, 0, "submitted", now);
    return send_attempt(now) ? id : 0;
}

void CommandManager::on_ack(const CommandAck& ack, std::uint8_t src_sys, std::uint8_t src_comp, MonoNs now) {
    const bool from_px4 = src_sys == cfg_.px4_sysid && src_comp == cfg_.px4_compid;
    // Each zero target field is unspecified; every nonzero field must match this GCS.
    const bool to_us = (ack.target_system == 0 || ack.target_system == cfg_.gcs_sysid) &&
                       (ack.target_component == 0 || ack.target_component == cfg_.gcs_compid);

    if (state_ == State::Idle) {
        const bool late = from_px4 && to_us && last_request_id_ != 0 && ack.command == last_command_;
        emit(last_request_id_, ack.command, 0, late ? "ack_late" : "ack_ignored", now, ack.result, ack.progress);
        return;
    }
    if (!from_px4 || !to_us || ack.command != pending_.command) {
        emit(pending_.request_id, ack.command, pending_.attempts, "ack_ignored", now, ack.result, ack.progress);
        return;
    }
    if (ack.result == kResultInProgress) {
        if (state_ == State::WaitingAck) {
            state_ = State::InProgress;
            pending_.first_progress_ns = now;
        }
        pending_.last_progress_ns = now;
        emit(pending_.request_id, pending_.command, pending_.attempts, "ack_in_progress", now, ack.result,
             ack.progress);
    } else if (ack.result == kResultAccepted) {
        finish(Outcome::Accepted, "ack_accepted", now, ack.result, ack.progress);
    } else {
        finish(Outcome::Rejected, "ack_rejected", now, ack.result, ack.progress);
    }
}

void CommandManager::tick(MonoNs now) {
    if (state_ == State::WaitingAck) {
        if (now >= pending_.last_sent_ns && now - pending_.last_sent_ns >= cfg_.ack_timeout_ns) {
            if (pending_.attempts < cfg_.max_sends) send_attempt(now);
            else finish(Outcome::Timeout, "final_timeout", now);
        }
    } else if (state_ == State::InProgress) {
        const bool idle = now >= pending_.last_progress_ns && now - pending_.last_progress_ns >= cfg_.progress_idle_ns;
        const bool total = now >= pending_.first_progress_ns && now - pending_.first_progress_ns >= cfg_.progress_total_ns;
        if (idle || total) finish(Outcome::Timeout, "final_timeout", now);
    }
}

void CommandManager::on_disconnect(MonoNs now) {
    disconnected_ = true;
    if (state_ != State::Idle) finish(Outcome::Unknown, "final_unknown", now); // khong tu phat lai
}
} // namespace cclient
