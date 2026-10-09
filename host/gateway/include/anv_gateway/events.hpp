// Events the gateway raises for later layers (ROS 2 bridge, dashboard).
#pragma once

#include <cstdint>

namespace anv::gw {

enum class EventType : uint8_t {
  RoleChanged,        // this host's role changed; role/term set
  LeaderLost,         // standby saw no master heartbeat for 250 ms; node_id = the master that vanished
  NodeAlive,          // node_id started (or resumed) sending
  NodeMissing,        // node_id silent for 300 ms
  NodeRebooted,       // node_id's uptime went backwards
  EstopSeen,          // an E-stop frame was seen (or sent by this host); node_id = 0 if unknown sender
  EstopReleased,      // ESTOP_RELEASE seen or sent
  NodeEvent,          // a NODE_EVENT frame arrived; code/detail/cleared set, node_id = sender
  EventGapDetected,   // a node's event sequence number skipped; node_id = sender, detail = gap size
  GatewayLinkLost,    // no message from the peer gateway for 1 s
  GatewayLinkUp,      // the link came back (or was first seen)
  DebugLevelChanged,  // a level/category/timer was applied; node_id = target (0 = all), detail = level
  DebugLevelAutoOff,  // an auto-off timer expired and reverted a node to its default level
  ChangeRefused,      // a requested change (debug level, rate) was refused; detail holds the reason code
  FlightRecorderSaved,  // the last 60 s of raw CAN was written to disk
};

struct GatewayEvent {
  EventType type = EventType::RoleChanged;
  int64_t time_ns = 0;  // CLOCK_MONOTONIC
  uint8_t node_id = 0;
  uint16_t term = 0;
  uint8_t role = 0;      // Role value, for RoleChanged
  uint16_t code = 0;     // errors.yaml code, 0 if this event has none (see firmware/generated/errors.h)
  uint16_t detail = 0;   // meaning depends on code/type, as for NODE_EVENT's detail field
  uint8_t severity = 0;  // rover_severity_t, valid when code != 0
  bool cleared = false;  // edge-triggered "condition cleared" (NODE_EVENT bit 7, or link/level restored)
};

const char* event_type_name(EventType t);

}  // namespace anv::gw
