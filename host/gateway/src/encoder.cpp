#include "anv_gateway/encoder.hpp"

#include "rover.h"

namespace anv::gw {

namespace {

CanFrame invalid_frame() {
  CanFrame f;
  f.id = kInvalidFrameId;
  return f;
}

// Pack with the generated function; any failure yields the invalid frame so callers never put
// half-built bytes on the bus.
#define ANV_PACK(FRAME, ID, LEN, PACK, SRC)                                \
  do {                                                                     \
    (FRAME).id = (ID);                                                     \
    (FRAME).dlc = (LEN);                                                   \
    if ((PACK)((FRAME).data.data(), &(SRC), (FRAME).data.size()) < 0) return invalid_frame(); \
  } while (0)

}  // namespace

CanFrame make_estop() {
  CanFrame f;
  struct rover_estop_t s;
  rover_estop_init(&s);
  s.magic = 0xE5;
  ANV_PACK(f, ROVER_ESTOP_FRAME_ID, ROVER_ESTOP_LENGTH, rover_estop_pack, s);
  return f;
}

CanFrame make_estop_release(uint8_t master_node_id, uint8_t master_term_low) {
  CanFrame f;
  struct rover_estop_release_t s;
  rover_estop_release_init(&s);
  s.master_node_id = master_node_id;
  s.master_term = master_term_low;
  ANV_PACK(f, ROVER_ESTOP_RELEASE_FRAME_ID, ROVER_ESTOP_RELEASE_LENGTH, rover_estop_release_pack, s);
  return f;
}

CanFrame make_sync(uint8_t cycle_counter) {
  CanFrame f;
  struct rover_sync_t s;
  rover_sync_init(&s);
  s.cycle_counter = cycle_counter;
  ANV_PACK(f, ROVER_SYNC_FRAME_ID, ROVER_SYNC_LENGTH, rover_sync_pack, s);
  return f;
}

CanFrame make_nmt(uint8_t target_node, uint8_t command, uint8_t master_node_id, uint8_t master_term_low) {
  CanFrame f;
  struct rover_nmt_t s;
  rover_nmt_init(&s);
  s.target_node = target_node;
  s.command = command;
  s.master_node_id = master_node_id;
  s.master_term = master_term_low;
  ANV_PACK(f, ROVER_NMT_FRAME_ID, ROVER_NMT_LENGTH, rover_nmt_pack, s);
  return f;
}

CanFrame make_host_hb(const HostHbMsg& hb) {
  CanFrame f;
#define ANV_HOST_HB(HOST, NAME, ID)                                              \
  do {                                                                           \
    struct rover_host_hb_##NAME##_t s;                                           \
    rover_host_hb_##NAME##_init(&s);                                             \
    s.role = hb.role;                                                            \
    s.tiebreak = hb.tie_break;                                                   \
    s.term = hb.term;                                                            \
    s.believed_master = hb.believed_master;                                      \
    s.flags = hb.flags;                                                          \
    s.uptime_s = hb.uptime_s;                                                    \
    ANV_PACK(f, ID, ROVER_HOST_HB_##HOST##_LENGTH, rover_host_hb_##NAME##_pack, s); \
    return f;                                                                    \
  } while (0)
  switch (hb.host_node) {
    case 1: ANV_HOST_HB(ORIN, orin, ROVER_HOST_HB_ORIN_FRAME_ID);
    case 2: ANV_HOST_HB(RPI, rpi, ROVER_HOST_HB_RPI_FRAME_ID);
    case 3: ANV_HOST_HB(TOOL, tool, ROVER_HOST_HB_TOOL_FRAME_ID);
    default: return invalid_frame();
  }
#undef ANV_HOST_HB
}

CanFrame make_lead_request(const LeadRequestMsg& req) {
  CanFrame f;
#define ANV_LEAD(HOST, NAME, ID)                                                       \
  do {                                                                                 \
    struct rover_lead_request_##NAME##_t s;                                            \
    rover_lead_request_##NAME##_init(&s);                                              \
    s.requested_leader = req.requested_leader;                                         \
    s.request_counter = req.request_counter;                                           \
    ANV_PACK(f, ID, ROVER_LEAD_REQUEST_##HOST##_LENGTH, rover_lead_request_##NAME##_pack, s); \
    return f;                                                                          \
  } while (0)
  switch (req.sender_node) {
    case 1: ANV_LEAD(ORIN, orin, ROVER_LEAD_REQUEST_ORIN_FRAME_ID);
    case 2: ANV_LEAD(RPI, rpi, ROVER_LEAD_REQUEST_RPI_FRAME_ID);
    case 3: ANV_LEAD(TOOL, tool, ROVER_LEAD_REQUEST_TOOL_FRAME_ID);
    default: return invalid_frame();
  }
#undef ANV_LEAD
}

CanFrame make_drive_cmd(const DriveCmdMsg& cmd) {
  CanFrame f;
#define ANV_DRIVE_CMD(N)                                                           \
  case N: {                                                                        \
    struct rover_drive##N##_cmd_t s;                                               \
    rover_drive##N##_cmd_init(&s);                                                 \
    s.seq = cmd.seq & 0xF;                                                         \
    s.sender_host = cmd.sender_host & 0x3;                                         \
    s.term_low = cmd.term_low & 0x3;                                               \
    s.mode = cmd.mode;                                                             \
    s.target_a = cmd.target_a;                                                     \
    s.target_b = cmd.target_b;                                                     \
    s.accel_limit = cmd.accel_limit;                                               \
    ANV_PACK(f, ROVER_DRIVE##N##_CMD_FRAME_ID, ROVER_DRIVE##N##_CMD_LENGTH, rover_drive##N##_cmd_pack, s); \
    return f;                                                                      \
  }
  switch (cmd.drive) {
    ANV_DRIVE_CMD(1)
    ANV_DRIVE_CMD(2)
    ANV_DRIVE_CMD(3)
    ANV_DRIVE_CMD(4)
    ANV_DRIVE_CMD(5)
    ANV_DRIVE_CMD(6)
    default: return invalid_frame();
  }
#undef ANV_DRIVE_CMD
}

CanFrame make_service_request(uint8_t node_id, uint8_t host, uint8_t opcode, uint8_t transaction_id,
                               uint16_t param_index, uint32_t value_raw) {
  CanFrame f;
#define ANV_SERVICE_REQ(N, HOST, NAME)                                                                        \
  do {                                                                                                        \
    struct rover_drive##N##_service_req_##NAME##_t s;                                                         \
    rover_drive##N##_service_req_##NAME##_init(&s);                                                           \
    s.opcode = opcode;                                                                                        \
    s.transaction_id = transaction_id;                                                                        \
    s.param_index = param_index;                                                                               \
    s.value_raw = value_raw;                                                                                   \
    ANV_PACK(f, ROVER_DRIVE##N##_SERVICE_REQ_##HOST##_FRAME_ID, ROVER_DRIVE##N##_SERVICE_REQ_##HOST##_LENGTH,  \
             rover_drive##N##_service_req_##NAME##_pack, s);                                                   \
    return f;                                                                                                  \
  } while (0)
#define ANV_SERVICE_REQ_DRIVE(N)              \
  case N:                                     \
    switch (host) {                          \
      case 1: ANV_SERVICE_REQ(N, ORIN, orin); \
      case 2: ANV_SERVICE_REQ(N, RPI, rpi);   \
      case 3: ANV_SERVICE_REQ(N, TOOL, tool); \
      default: return invalid_frame();        \
    }
  switch (node_id - 3) {  // node 4-9 -> drive 1-6
    ANV_SERVICE_REQ_DRIVE(1)
    ANV_SERVICE_REQ_DRIVE(2)
    ANV_SERVICE_REQ_DRIVE(3)
    ANV_SERVICE_REQ_DRIVE(4)
    ANV_SERVICE_REQ_DRIVE(5)
    ANV_SERVICE_REQ_DRIVE(6)
    default: return invalid_frame();
  }
#undef ANV_SERVICE_REQ_DRIVE
#undef ANV_SERVICE_REQ
}

CanFrame make_node_hb(const NodeHbMsg& hb) {
  CanFrame f;
#define ANV_NODE_HB(NAME, ID, LEN)                           \
  do {                                                       \
    struct rover_##NAME##_node_hb_t s;                       \
    rover_##NAME##_node_hb_init(&s);                         \
    s.state = hb.state;                                      \
    s.node_type = hb.node_type;                              \
    s.protocol_version = hb.protocol_version;                \
    s.can_error_state = hb.can_error_state;                  \
    s.bootloader_running = hb.bootloader_running;            \
    s.unsaved_param_changes = hb.unsaved_param_changes;      \
    s.debug_level = hb.debug_level;                          \
    s.fw_build_id = hb.fw_build_id;                          \
    s.uptime_s = hb.uptime_s;                                \
    ANV_PACK(f, ID, LEN, rover_##NAME##_node_hb_pack, s);    \
    return f;                                                \
  } while (0)
  switch (hb.node_id) {
    case 4: ANV_NODE_HB(drive1, ROVER_DRIVE1_NODE_HB_FRAME_ID, ROVER_DRIVE1_NODE_HB_LENGTH);
    case 5: ANV_NODE_HB(drive2, ROVER_DRIVE2_NODE_HB_FRAME_ID, ROVER_DRIVE2_NODE_HB_LENGTH);
    case 6: ANV_NODE_HB(drive3, ROVER_DRIVE3_NODE_HB_FRAME_ID, ROVER_DRIVE3_NODE_HB_LENGTH);
    case 7: ANV_NODE_HB(drive4, ROVER_DRIVE4_NODE_HB_FRAME_ID, ROVER_DRIVE4_NODE_HB_LENGTH);
    case 8: ANV_NODE_HB(drive5, ROVER_DRIVE5_NODE_HB_FRAME_ID, ROVER_DRIVE5_NODE_HB_LENGTH);
    case 9: ANV_NODE_HB(drive6, ROVER_DRIVE6_NODE_HB_FRAME_ID, ROVER_DRIVE6_NODE_HB_LENGTH);
    case 20: ANV_NODE_HB(fpga, ROVER_FPGA_NODE_HB_FRAME_ID, ROVER_FPGA_NODE_HB_LENGTH);
    default: return invalid_frame();
  }
#undef ANV_NODE_HB
}

CanFrame make_drive_fb_a(const DriveFbAMsg& fb) {
  CanFrame f;
#define ANV_FB_A(N)                                                                      \
  case N: {                                                                              \
    struct rover_drive##N##_fb_a_t s;                                                    \
    rover_drive##N##_fb_a_init(&s);                                                      \
    s.state = fb.state;                                                                  \
    s.fault_flags = fb.fault_flags;                                                      \
    s.speed_a = fb.speed_a;                                                              \
    s.speed_b = fb.speed_b;                                                              \
    s.current_a = fb.current_a;                                                          \
    s.current_b = fb.current_b;                                                          \
    ANV_PACK(f, ROVER_DRIVE##N##_FB_A_FRAME_ID, ROVER_DRIVE##N##_FB_A_LENGTH, rover_drive##N##_fb_a_pack, s); \
    return f;                                                                            \
  }
  switch (fb.drive) {
    ANV_FB_A(1)
    ANV_FB_A(2)
    ANV_FB_A(3)
    ANV_FB_A(4)
    ANV_FB_A(5)
    ANV_FB_A(6)
    default: return invalid_frame();
  }
#undef ANV_FB_A
}

}  // namespace anv::gw
