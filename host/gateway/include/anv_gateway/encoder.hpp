// Builders for every frame the gateway transmits. Packing goes through the generated C code.
#pragma once

#include <cstdint>

#include "anv_gateway/can_bus.hpp"
#include "anv_gateway/messages.hpp"

namespace anv::gw {

CanFrame make_estop();
CanFrame make_estop_release(uint8_t master_node_id, uint8_t master_term_low);
CanFrame make_sync(uint8_t cycle_counter);
CanFrame make_nmt(uint8_t target_node, uint8_t command, uint8_t master_node_id, uint8_t master_term_low);
// host_node 1 (Orin), 2 (RPi) or 3 (tool). Returns an empty frame (dlc 0, id 0xFFFF) for other values.
CanFrame make_host_hb(const HostHbMsg& hb);
CanFrame make_lead_request(const LeadRequestMsg& req);
// msg.drive in 1-6; other values return an empty frame (id 0xFFFF).
CanFrame make_drive_cmd(const DriveCmdMsg& cmd);
// Master only. target_node 0 = all nodes.
CanFrame make_debug_level(const DebugLevelMsg& msg);
// msg.node_id must be a drive node (4-9); msg.host in 1-3; msg.is_response must be false.
// Any other combination (e.g. the FPGA, which has no SERVICE_REQ messages) returns an empty frame.
CanFrame make_service_req(const ServiceMsg& msg);

// Packers for node-side frames, used by tests and by anything that simulates a node in C++.
CanFrame make_node_hb(const NodeHbMsg& hb);  // node_id 4-9 (drive) or 20 (FPGA)
CanFrame make_drive_fb_a(const DriveFbAMsg& fb);
CanFrame make_node_event(const NodeEventMsg& e);  // node_id 4-9 (drive) or 20 (FPGA)

constexpr uint16_t kInvalidFrameId = 0xFFFF;

}  // namespace anv::gw
