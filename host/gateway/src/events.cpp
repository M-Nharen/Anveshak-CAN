#include "anv_gateway/events.hpp"

namespace anv::gw {

const char* event_type_name(EventType t) {
  switch (t) {
    case EventType::RoleChanged: return "ROLE_CHANGED";
    case EventType::LeaderLost: return "LEADER_LOST";
    case EventType::NodeAlive: return "NODE_ALIVE";
    case EventType::NodeMissing: return "NODE_MISSING";
    case EventType::NodeRebooted: return "NODE_REBOOTED";
    case EventType::EstopSeen: return "ESTOP_SEEN";
    case EventType::EstopReleased: return "ESTOP_RELEASED";
    case EventType::NodeEvent: return "NODE_EVENT";
    case EventType::EventGapDetected: return "EVENT_GAP_DETECTED";
    case EventType::GatewayLinkLost: return "GATEWAY_LINK_LOST";
    case EventType::GatewayLinkUp: return "GATEWAY_LINK_UP";
    case EventType::DebugLevelChanged: return "DEBUG_LEVEL_CHANGED";
    case EventType::DebugLevelAutoOff: return "DEBUG_LEVEL_AUTO_OFF";
    case EventType::ChangeRefused: return "CHANGE_REFUSED";
    case EventType::FlightRecorderSaved: return "FLIGHT_RECORDER_SAVED";
  }
  return "?";
}

}  // namespace anv::gw
