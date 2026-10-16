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
    case EventType::NodeIdAssigned: return "NODE_ID_ASSIGNED";
  }
  return "?";
}

}  // namespace anv::gw
