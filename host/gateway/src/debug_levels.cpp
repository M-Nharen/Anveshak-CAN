#include "anv_gateway/debug_levels.hpp"

#include "anv_gateway/protocol.hpp"
#include "errors.h"

namespace anv::gw {

namespace {
constexpr int64_t kMinuteNs = 60 * kNsPerSec;
}  // namespace

bool DebugLevelManager::request(uint8_t target_node, uint8_t level, uint8_t category_mask,
                                 uint16_t auto_off_min, int64_t now_ns, std::vector<GatewayEvent>& events) {
  if (target_node > kMaxNodeId || level > 3) return false;

  const double projected = estimate_ ? estimate_(target_node, level) : 0.0;
  if (projected > kBusLoadRefusalFraction) {
    GatewayEvent e;
    e.type = EventType::ChangeRefused;
    e.time_ns = now_ns;
    e.node_id = target_node;
    e.code = ROVER_ERR_CHANGE_REFUSED;
    e.severity = static_cast<uint8_t>(ROVER_SEVERITY_WARNING);
    e.detail = static_cast<uint16_t>(projected * 1000);  // per-mille, for the dashboard
    events.push_back(e);
    return false;
  }

  const int64_t auto_off_at = auto_off_min == 0 ? 0 : now_ns + static_cast<int64_t>(auto_off_min) * kMinuteNs;
  if (target_node == 0) {
    rover_ = DebugNodeState{level, category_mask, auto_off_at};
    overridden_.fill(false);  // a whole-rover command clears any per-node override
  } else {
    nodes_[target_node] = DebugNodeState{level, category_mask, auto_off_at};
    overridden_[target_node] = true;
  }

  GatewayEvent e;
  e.type = EventType::DebugLevelChanged;
  e.time_ns = now_ns;
  e.node_id = target_node;
  e.code = ROVER_ERR_CONTROL_ACTION;
  e.severity = static_cast<uint8_t>(ROVER_SEVERITY_INFO);
  e.detail = level;
  events.push_back(e);
  return true;
}

void DebugLevelManager::tick(int64_t now_ns, std::vector<GatewayEvent>& events,
                              const std::function<void(uint8_t, uint8_t, uint8_t)>& apply) {
  auto expire = [&](uint8_t node_id, DebugNodeState& st) {
    if (st.auto_off_at_ns == 0 || now_ns < st.auto_off_at_ns) return;
    st = DebugNodeState{};
    if (node_id != 0) overridden_[node_id] = false;
    GatewayEvent e;
    e.type = EventType::DebugLevelAutoOff;
    e.time_ns = now_ns;
    e.node_id = node_id;
    events.push_back(e);
    if (apply) apply(node_id, 0, kDebugCategoryAll);
  };
  expire(0, rover_);
  for (uint8_t n = 1; n <= kMaxNodeId; ++n)
    if (overridden_[n]) expire(n, nodes_[n]);
}

uint8_t DebugLevelManager::level_of(uint8_t node_id) const {
  if (node_id != 0 && node_id <= kMaxNodeId && overridden_[node_id]) return nodes_[node_id].level;
  return rover_.level;
}

uint8_t DebugLevelManager::category_mask_of(uint8_t node_id) const {
  if (node_id != 0 && node_id <= kMaxNodeId && overridden_[node_id]) return nodes_[node_id].category_mask;
  return rover_.category_mask;
}

bool DebugLevelManager::any_above_normal() const {
  if (rover_.level > 1) return true;
  for (uint8_t n = 1; n <= kMaxNodeId; ++n)
    if (overridden_[n] && nodes_[n].level > 1) return true;
  return false;
}

int64_t DebugLevelManager::remaining_auto_off_ns(int64_t now_ns) const {
  int64_t best = 0;
  auto consider = [&](const DebugNodeState& st) {
    if (st.auto_off_at_ns == 0) return;
    const int64_t left = st.auto_off_at_ns - now_ns;
    if (left > 0 && (best == 0 || left < best)) best = left;
  };
  consider(rover_);
  for (uint8_t n = 1; n <= kMaxNodeId; ++n)
    if (overridden_[n]) consider(nodes_[n]);
  return best;
}

}  // namespace anv::gw
