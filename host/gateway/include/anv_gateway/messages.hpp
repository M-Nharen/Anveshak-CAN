// Typed C++ views of every DBC message. Field values are the raw wire integers (the scaling
// factors are in defs/rover.dbc); the helpers at the bottom apply the documented scales.
// The numbers come out of the generated C code, never from hand-written byte shuffling.
#pragma once

#include <array>
#include <cstdint>
#include <string>
#include <variant>

namespace anv::gw {

struct EstopMsg { uint8_t magic = 0; };
struct EstopReleaseMsg { uint8_t master_node_id = 0; uint8_t master_term = 0; };
struct SyncMsg { uint8_t cycle_counter = 0; };
struct NmtMsg { uint8_t target_node = 0; uint8_t command = 0; uint8_t master_node_id = 0; uint8_t master_term = 0; };
struct DebugLevelMsg {
  uint8_t target_node = 0;
  uint8_t level = 0;
  uint8_t category_mask = 0;
  uint16_t auto_off_timer_min = 0;
  uint8_t master_node_id = 0;
  uint8_t master_term = 0;
};

struct HostHbMsg {
  uint8_t host_node = 0;  // sender: 1 Orin, 2 RPi, 3 tool
  uint8_t role = 0;
  uint8_t tie_break = 0;
  uint16_t term = 0;
  uint8_t believed_master = 0;
  uint8_t flags = 0;
  uint16_t uptime_s = 0;
};
struct LeadRequestMsg {
  uint8_t sender_node = 0;  // 1 Orin, 2 RPi, 3 tool
  uint8_t requested_leader = 0;
  uint8_t request_counter = 0;
};

struct DriveCmdMsg {
  uint8_t drive = 0;  // 1-6
  uint8_t seq = 0;
  uint8_t sender_host = 0;
  uint8_t term_low = 0;
  uint8_t mode = 0;
  int16_t target_a = 0;
  int16_t target_b = 0;
  uint16_t accel_limit = 0;
};
struct DriveFbAMsg {
  uint8_t drive = 0;
  uint8_t state = 0;
  uint8_t fault_flags = 0;
  int16_t speed_a = 0;
  int16_t speed_b = 0;
  uint8_t current_a = 0;
  uint8_t current_b = 0;
};
struct DriveFbBMsg { uint8_t drive = 0; int32_t encoder_a = 0; int32_t encoder_b = 0; };
struct DriveTelem0Msg {
  uint8_t drive = 0;
  uint16_t supply_voltage_mv = 0;
  int8_t temp_driver_a = 0;
  int8_t temp_driver_b = 0;
  int8_t temp_mcu = 0;
  uint8_t tec = 0;
  uint8_t rec = 0;
  uint8_t bus_off_recovery_count = 0;
};
struct DriveTelem1Msg {
  uint8_t drive = 0;
  int16_t duty_a = 0;
  int16_t duty_b = 0;
  uint8_t watchdog_reset_count = 0;
  uint8_t last_reset_cause = 0;
  uint16_t commands_missed = 0;
};
struct DriveDebugMsg {  // DRIVE_DEBUG_A (channel A) or _B (channel B)
  uint8_t drive = 0;
  bool channel_b = false;
  int16_t setpoint = 0;
  int16_t measured = 0;
  int16_t integrator = 0;
  int16_t output = 0;
};

struct NodeHbMsg {
  uint8_t node_id = 0;
  uint8_t state = 0;
  uint8_t node_type = 0;
  uint8_t protocol_version = 0;
  uint8_t can_error_state = 0;
  uint8_t bootloader_running = 0;
  uint8_t unsaved_param_changes = 0;
  uint8_t debug_level = 0;
  uint16_t fw_build_id = 0;
  uint16_t uptime_s = 0;
};
struct NodeEventMsg {
  uint8_t node_id = 0;
  uint8_t severity = 0;
  uint8_t cleared = 0;
  uint16_t error_code = 0;
  uint8_t event_seq = 0;
  uint16_t detail = 0;
  uint16_t uptime_0_1_s = 0;
};
struct FpgaSensorsMsg {  // FPGA_SENSORS_A (sensors 1-4) or _B (sensors 5-8)
  bool second_half = false;
  std::array<uint16_t, 4> values{};
};

struct ServiceMsg {
  uint8_t node_id = 0;       // the node being asked / answering
  uint8_t host = 0;          // 1 Orin, 2 RPi, 3 tool: the requesting host
  bool is_response = false;
  uint8_t opcode = 0;
  uint8_t status = 0;  // responses only
  uint8_t transaction_id = 0;
  uint16_t param_index = 0;
  uint32_t value_raw = 0;
};
// Automatic node IDs (docs/architecture.md "Automatic node IDs"). MAC is the board's 48-bit
// factory MAC as one integer, least-significant byte first on the wire.
struct NodeIdRequestMsg { uint64_t mac = 0; uint8_t node_type = 0; uint8_t hw_target = 0; };
struct NodeIdAssignMsg {
  uint8_t host = 0;  // sender: 1 Orin, 2 RPi, 3 tool
  uint64_t mac = 0;
  uint8_t assigned_id = 0;
  uint8_t flags = 0;
};

// Bench/debug text (class 7, lowest priority): up to 7 ASCII characters. Host -> node at
// 0x700 + 8 * node + 2/3/4 (from Orin / RPi / tool; node 0 = every node), node -> host at sub 5.
constexpr size_t kTextMaxLen = 7;
struct TextToNodeMsg {
  uint8_t host = 0;  // sender: 1 Orin, 2 RPi, 3 tool
  uint8_t node = 0;  // target, 0 = every node
  uint8_t seq = 0;   // 1-255
  std::string text;
};
struct TextFromNodeMsg {
  uint8_t node = 0;
  uint8_t seq = 0;  // the host text it echoes back, or 0 for text the node sent on its own
  std::string text;
};

struct BootMsg {  // raw bytes: B4/A3 define the ISO-TP layer on top
  uint8_t node_id = 0;
  bool node_to_host = false;
  std::array<uint8_t, 8> bytes{};
};

// Keep this order in sync with kMessageNames in decoder.cpp.
using Message = std::variant<std::monostate, EstopMsg, EstopReleaseMsg, SyncMsg, NmtMsg, DebugLevelMsg, HostHbMsg,
                             LeadRequestMsg, DriveCmdMsg, DriveFbAMsg, DriveFbBMsg, DriveTelem0Msg, DriveTelem1Msg,
                             DriveDebugMsg, NodeHbMsg, NodeEventMsg, FpgaSensorsMsg, ServiceMsg, BootMsg,
                             NodeIdRequestMsg, NodeIdAssignMsg, TextToNodeMsg, TextFromNodeMsg>;

// --- Scaled views (units from the DBC comments) ---
inline double speed_rad_s(int16_t raw) { return raw * 0.01; }
inline double current_a(uint8_t raw) { return raw * 0.2; }
inline double duty_percent(int16_t raw) { return raw * 0.01; }

}  // namespace anv::gw
