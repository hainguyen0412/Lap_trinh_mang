#pragma once
// Dong goi cac ban tin uplink cua GCS thanh raw bytes MAVLink 2 (khong ky).
// Header nay KHONG include thu vien MAVLink C; chi file .cpp moi dung.
#include "mavlink_gateway/contracts.hpp"
#include <array>
#include <cstdint>

namespace cclient {
struct GcsId {
    std::uint8_t sysid = 255;
    std::uint8_t compid = 190;
};

mavlink_gateway::Bytes build_gcs_heartbeat(const GcsId& gcs);

mavlink_gateway::Bytes build_command_long(const GcsId& gcs, std::uint8_t target_system,
                                          std::uint8_t target_component, std::uint16_t command,
                                          const std::array<float, 7>& params,
                                          std::uint8_t confirmation);
} // namespace cclient
