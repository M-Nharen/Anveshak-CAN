// Debug-level bookkeeping: docs/architecture.md "Debug levels: switching debugging on and off".
// Pure logic, like leadership.hpp: no bus, no clock of its own. The bus-load guard is injected as
// a LoadEstimator so tests can exercise the refusal/auto-off rules without modelling the whole DBC;
// gateway.cpp wires in an estimator built from NodeRegistry::expected_hz for the real executable.
#pragma once

#include <array>
#include <cstdint>
#include <functional>
#include <vector>

#include "anv_gateway/events.hpp"

namespace anv::gw {

// Refuse a level/category change that would push the projected bus load over this fraction.
constexpr double kBusLoadRefusalFraction = 0.5;

constexpr uint8_t kDebugCategoryAll = 0x0F;   // bits: events, telemetry, debug pages, error counts
constexpr uint8_t kMaxNodeId = 31;            // DEBUG_LEVEL target_node range (0 = all nodes)

struct DebugNodeState {
  uint8_t level = 0;
  uint8_t category_mask = kDebugCategoryAll;
  int64_t auto_off_at_ns = 0;  // 0 = no timer (until changed)
};

// Projected bus-load fraction (0..1) if `target_node` (0 = the whole rover) moved to `level`,
// holding every other node at whatever it is currently reporting.
using LoadEstimator = std::function<double(uint8_t target_node, uint8_t level)>;

class DebugLevelManager {
 public:
  explicit DebugLevelManager(LoadEstimator estimator) : estimate_(std::move(estimator)) {}

  // Applies the change and raises DebugLevelChanged, or refuses it (ChangeRefused) if the
  // projected load would exceed kBusLoadRefusalFraction. Returns false on refusal or a bad
  // target_node. auto_off_min: minutes until auto-revert to level 0, 0 = until changed.
  bool request(uint8_t target_node, uint8_t level, uint8_t category_mask, uint16_t auto_off_min,
               int64_t now_ns, std::vector<GatewayEvent>& events);

  // Call at least once a second. Reverts any node (or the whole rover) whose auto-off timer has
  // expired to level 0 and raises DebugLevelAutoOff; `apply` is called once per reverted entry
  // (node 0 = the whole rover) so the caller can transmit DEBUG_LEVEL.
  void tick(int64_t now_ns, std::vector<GatewayEvent>& events,
            const std::function<void(uint8_t node, uint8_t level, uint8_t category_mask)>& apply);

  // node_id 0 asks about the whole-rover setting; any other node reports its own override if it
  // has one, else the whole-rover setting (matches what a node without its own DEBUG_LEVEL would do).
  uint8_t level_of(uint8_t node_id) const;
  uint8_t category_mask_of(uint8_t node_id) const;

  // True while any node or the whole rover is above level 1 ("Normal"): drives the dashboard's
  // "DEBUG ON" banner.
  bool any_above_normal() const;
  // Time left on the soonest active auto-off timer, 0 if none is running.
  int64_t remaining_auto_off_ns(int64_t now_ns) const;

 private:
  DebugNodeState rover_;                           // target_node == 0
  std::array<DebugNodeState, kMaxNodeId + 1> nodes_{};
  std::array<bool, kMaxNodeId + 1> overridden_{};  // true once node_id got its own DEBUG_LEVEL
  LoadEstimator estimate_;
};

}  // namespace anv::gw
