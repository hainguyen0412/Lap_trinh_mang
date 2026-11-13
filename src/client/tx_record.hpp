#pragma once
#include "mavlink_gateway/contracts.hpp"

namespace cclient {
struct TxTag {
    std::uint64_t id = 0;
    mavlink_gateway::MonoNs queued_at_ns = 0;
};
struct SendCompletion {
    TxTag tag;
    mavlink_gateway::MonoNs completed_at_ns = 0;
    std::size_t frame_bytes = 0;
    mavlink_gateway::DeliveryStatus status = mavlink_gateway::DeliveryStatus::SentToSocket;
};
} // namespace cclient
