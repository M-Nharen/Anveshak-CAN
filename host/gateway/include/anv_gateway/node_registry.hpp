// Node registry: per node alive/missing (300 ms), state, firmware build, uptime (reboot
// detection), CAN error counters, latest decoded feedback, and measured vs expected message
// rates. Single-threaded: the gateway guards it with its state mutex.
#pragma once

#include <array>
#include <cstdint>
#include <map>
#include <string>
#include <unordered_map>
#include <vector>

#include "anv_gateway/decoder.hpp"
#include "anv_gateway/events.hpp"

namespace anv::gw {

struct NodeInfo {
  uint8_t node_id = 0;
  std::string name;
  bool seen = false;   // at least one frame since the gateway started
  bool alive = false;  // a frame within the last 300 ms
  int64_t last_seen_ns = 0;

  bool has_hb = false;  // a NODE_HB has arrived (uptime and build fields are valid)
  uint8_t state = 0;  // NodeState, from the latest NODE_HB or DRIVE_FB_A
  uint8_t node_type = 0;
  uint8_t protocol_version = 0;
  uint8_t can_error_state = 0;
  bool bootloader_running = false;
  bool unsaved_param_changes = false;
  uint8_t debug_level = 0;
  uint16_t fw_build_id = 0;
  uint16_t uptime_s = 0;
  uint32_t reboot_count = 0;  // times the uptime went backwards

  uint8_t tec = 0;  // CAN error counters from DRIVE_TELEM_0
  uint8_t rec = 0;

  // Latest decoded values (drive nodes / FPGA); the has_* flags say whether one has arrived.
  bool has_fb_a = false, has_fb_b = false, has_telem0 = false, has_telem1 = false, has_sensors = false;
  DriveFbAMsg fb_a;
  DriveFbBMsg fb_b;
  DriveTelem0Msg telem0;
  DriveTelem1Msg telem1;
  std::array<uint16_t, 8> sensors{};
};

struct RateInfo {
  uint16_t id = 0;
  uint8_t node_id = 0;
  double measured_hz = 0;  // frames per second over the last full 1 s window
  double expected_hz = 0;  // 0 = event-driven or not expected at this debug level
  bool low = false;        // expected > 0 and measured below 80 % of it
};

class NodeRegistry {
 public:
  NodeRegistry();  // pre-seeds the nodes the rover is built with (drive 1-6, FPGA) as "never seen"

  // Feed every decoded frame. Raises NodeAlive / NodeRebooted into `events`.
  void on_decoded(const Decoded& d, std::vector<GatewayEvent>& events);
  // Call at least every ~10 ms: rolls rate windows and raises NodeMissing.
  void tick(int64_t now_ns, std::vector<GatewayEvent>& events);

  std::vector<NodeInfo> snapshot(int64_t now_ns) const;
  std::vector<RateInfo> rates() const;

  // Expected rate of a node-originated frame ID given the node's debug level (docs: message catalog).
  static double expected_hz(uint16_t id, uint8_t debug_level);

 private:
  struct RateStat {
    bool started = false;
    int64_t window_start_ns = 0;
    uint32_t count = 0;
    double hz = 0;
  };
  NodeInfo& entry(uint8_t node_id);
  void touch(uint8_t node_id, int64_t rx_ns, std::vector<GatewayEvent>& events);

  std::map<uint8_t, NodeInfo> nodes_;
  std::unordered_map<uint16_t, RateStat> rates_;
};

std::string node_name(uint8_t node_id);

}  // namespace anv::gw
