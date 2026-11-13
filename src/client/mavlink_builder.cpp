#include "mavlink_builder.hpp"
#include <common/mavlink.h>
#include <mutex>

namespace cclient {
namespace {
// Thu vien MAVLink C giu bo dem SEQ tinh; khoa de nhieu thread khong dua nhau.
std::mutex g_mutex;

mavlink_gateway::Bytes to_bytes(const mavlink_message_t& msg) {
    std::uint8_t buf[MAVLINK_MAX_PACKET_LEN];
    const std::uint16_t n = mavlink_msg_to_send_buffer(buf, &msg);
    return mavlink_gateway::Bytes(buf, buf + n);
}
} // namespace

mavlink_gateway::Bytes build_gcs_heartbeat(const GcsId& gcs) {
    std::lock_guard<std::mutex> lock(g_mutex);
    mavlink_message_t msg{};
    mavlink_msg_heartbeat_pack(gcs.sysid, gcs.compid, &msg, MAV_TYPE_GCS, MAV_AUTOPILOT_INVALID,
                               0, 0, MAV_STATE_ACTIVE);
    return to_bytes(msg);
}

mavlink_gateway::Bytes build_command_long(const GcsId& gcs, std::uint8_t target_system,
                                          std::uint8_t target_component, std::uint16_t command,
                                          const std::array<float, 7>& p,
                                          std::uint8_t confirmation) {
    std::lock_guard<std::mutex> lock(g_mutex);
    mavlink_message_t msg{};
    mavlink_msg_command_long_pack(gcs.sysid, gcs.compid, &msg, target_system, target_component,
                                  command, confirmation, p[0], p[1], p[2], p[3], p[4], p[5], p[6]);
    return to_bytes(msg);
}
} // namespace cclient
