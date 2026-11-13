#pragma once
#include "contracts.hpp"
#include <mutex>
#include <tuple>

namespace mavlink_gateway {

// One instance per owning worker; source IDs isolate the TCP buffers it owns.
class Parser final : public IParser {
public:
    explicit Parser(std::size_t tcp_buffer_limit = 65536);
    ParseBatch consume(const ReceiveEvent& event) override;
    void reset(SourceId source_id) override;
private:
    struct TcpState { SourceRef source; Bytes bytes; };
    std::size_t tcp_buffer_limit_;
    std::map<SourceId, TcpState> tcp_;
    static void extract(Bytes& bytes, const ReceiveEvent& event, bool datagram, ParseBatch& result);
};

class Decoder final : public IDecoder {
public:
    std::optional<DecodedMessage> decode(const FrameRecord& frame) const override;
};

class Inspector final : public IInspector {
public:
    void on_frame(const FrameRecord& frame, const std::optional<DecodedMessage>& decoded) override;
    void on_issue(const ParseIssue& issue) override;
    InspectorSnapshot snapshot() const override;
    // Release disconnected-source state; cumulative counters remain available.
    void forget_source(SourceId source_id);
private:
    using Key = std::tuple<SourceId, std::uint8_t, std::uint8_t>;
    struct State { SourceStats stats; std::optional<std::uint8_t> seq; };
    mutable std::mutex mutex_;
    InspectorSnapshot totals_;
    std::map<Key, State> sources_;
};

const char* issue_name(ParseIssueCode code) noexcept;
const char* delivery_name(DeliveryStatus status) noexcept;
const char* mode_name(MeasurementMode mode) noexcept;

} // namespace mavlink_gateway
