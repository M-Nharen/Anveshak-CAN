// Events the gateway raises for later layers (ROS 2 bridge, dashboard).
#pragma once

#include <cstdint>

namespace anv::gw {

enum class EventType : uint8_t {
  RoleChanged,    // this host's role changed; role/term set
  LeaderLost,     // standby saw no master heartbeat for 250 ms; node_id = the master that vanished
  NodeAlive,      // node_id started (or resumed) sending
  NodeMissing,    // node_id silent for 300 ms
  NodeRebooted,   // node_id's uptime went backwards
  EstopSeen,      // an E-stop frame was seen (or sent by this host); node_id = 0 if unknown sender
  EstopReleased,  // ESTOP_RELEASE seen or sent
};

struct GatewayEvent {
  EventType type = EventType::RoleChanged;
  int64_t time_ns = 0;  // CLOCK_MONOTONIC
  uint8_t node_id = 0;
  uint16_t term = 0;
  uint8_t role = 0;  // Role value, for RoleChanged
};

const char* event_type_name(EventType t);

}  // namespace anv::gw
