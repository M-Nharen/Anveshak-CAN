// Pure-logic tests for DebugLevelManager: no bus, no threads, a fake load estimator so the
// refusal/auto-off rules are exercised deterministically (docs/architecture.md "Debug levels").
#include "anv_gateway/debug_levels.hpp"
#include "test_util.hpp"

using namespace anv::gw;
using namespace t;

namespace {

size_t count(const std::vector<GatewayEvent>& ev, EventType type) {
  size_t n = 0;
  for (const auto& e : ev)
    if (e.type == type) ++n;
  return n;
}

}  // namespace

TEST(a_change_within_budget_is_applied) {
  DebugLevelManager mgr([](uint8_t, uint8_t) { return 0.2; });
  std::vector<GatewayEvent> ev;
  CHECK(mgr.request(0, 1, kDebugCategoryAll, 0, 1000, ev));
  CHECK_EQ(mgr.level_of(0), 1u);
  CHECK_EQ(count(ev, EventType::DebugLevelChanged), 1u);
  CHECK_EQ(ev[0].node_id, 0u);
  CHECK_EQ(ev[0].detail, 1u);
}

TEST(a_change_over_50_percent_is_refused) {
  DebugLevelManager mgr([](uint8_t, uint8_t level) { return level >= 2 ? 0.6 : 0.1; });
  std::vector<GatewayEvent> ev;
  CHECK(!mgr.request(0, 2, kDebugCategoryAll, 0, 1000, ev));
  CHECK_EQ(mgr.level_of(0), 0u);  // unchanged
  CHECK_EQ(count(ev, EventType::ChangeRefused), 1u);
  CHECK_EQ(count(ev, EventType::DebugLevelChanged), 0u);
}

TEST(a_per_node_request_does_not_affect_other_nodes) {
  DebugLevelManager mgr([](uint8_t, uint8_t) { return 0.1; });
  std::vector<GatewayEvent> ev;
  CHECK(mgr.request(5, 2, kDebugCategoryAll, 0, 1000, ev));
  CHECK_EQ(mgr.level_of(5), 2u);
  CHECK_EQ(mgr.level_of(6), 0u);  // never touched: still the whole-rover default
  CHECK_EQ(mgr.level_of(0), 0u);
}

TEST(a_whole_rover_request_clears_per_node_overrides) {
  DebugLevelManager mgr([](uint8_t, uint8_t) { return 0.1; });
  std::vector<GatewayEvent> ev;
  CHECK(mgr.request(5, 2, kDebugCategoryAll, 0, 1000, ev));
  CHECK(mgr.request(0, 1, kDebugCategoryAll, 0, 1000, ev));
  CHECK_EQ(mgr.level_of(5), 1u);  // the override is gone; node 5 now follows the rover
  CHECK_EQ(mgr.level_of(0), 1u);
}

TEST(a_refused_request_does_not_touch_other_nodes) {
  DebugLevelManager mgr([](uint8_t node, uint8_t level) { return node == 7 && level >= 2 ? 0.9 : 0.1; });
  std::vector<GatewayEvent> ev;
  CHECK(mgr.request(3, 2, kDebugCategoryAll, 0, 1000, ev));
  CHECK(!mgr.request(7, 2, kDebugCategoryAll, 0, 1000, ev));
  CHECK_EQ(mgr.level_of(3), 2u);
  CHECK_EQ(mgr.level_of(7), 0u);
}

TEST(bad_arguments_are_rejected_without_an_event) {
  DebugLevelManager mgr([](uint8_t, uint8_t) { return 0.0; });
  std::vector<GatewayEvent> ev;
  CHECK(!mgr.request(32, 1, kDebugCategoryAll, 0, 1000, ev));  // node id out of range
  CHECK(!mgr.request(0, 4, kDebugCategoryAll, 0, 1000, ev));   // level out of range
  CHECK(ev.empty());
}

TEST(an_auto_off_timer_reverts_the_level_after_it_expires) {
  DebugLevelManager mgr([](uint8_t, uint8_t) { return 0.1; });
  std::vector<GatewayEvent> ev;
  const int64_t t0 = 1000;
  CHECK(mgr.request(0, 3, kDebugCategoryAll, 1 /*minute*/, t0, ev));
  CHECK_EQ(mgr.level_of(0), 3u);
  CHECK(mgr.remaining_auto_off_ns(t0) > 0);

  ev.clear();
  int applied_node = -1, applied_level = -1;
  mgr.tick(t0 + 59 * kNsPerSec, ev, [&](uint8_t n, uint8_t l, uint8_t) { applied_node = n; applied_level = l; });
  CHECK_EQ(mgr.level_of(0), 3u);  // not expired yet
  CHECK(ev.empty());

  mgr.tick(t0 + 61 * kNsPerSec, ev, [&](uint8_t n, uint8_t l, uint8_t) { applied_node = n; applied_level = l; });
  CHECK_EQ(mgr.level_of(0), 0u);
  CHECK_EQ(count(ev, EventType::DebugLevelAutoOff), 1u);
  CHECK_EQ(applied_node, 0);
  CHECK_EQ(applied_level, 0);
}

TEST(a_whole_rover_auto_off_does_not_touch_an_active_per_node_override) {
  DebugLevelManager mgr([](uint8_t, uint8_t) { return 0.1; });
  std::vector<GatewayEvent> ev;
  CHECK(mgr.request(0, 1, kDebugCategoryAll, 1 /*minute*/, 1000, ev));  // whole-rover, with a timer
  CHECK(mgr.request(4, 2, kDebugCategoryAll, 0, 1000, ev));             // node 4's own override, no timer
  ev.clear();
  mgr.tick(1000 + 61 * kNsPerSec, ev, [](uint8_t, uint8_t, uint8_t) {});
  CHECK_EQ(mgr.level_of(0), 0u);  // the rover's timer expired
  CHECK_EQ(mgr.level_of(4), 2u);  // node 4's own override has no timer and is untouched
}

TEST(any_above_normal_reflects_both_the_rover_and_per_node_levels) {
  DebugLevelManager mgr([](uint8_t, uint8_t) { return 0.1; });
  std::vector<GatewayEvent> ev;
  CHECK(!mgr.any_above_normal());
  CHECK(mgr.request(9, 2, kDebugCategoryAll, 0, 1000, ev));
  CHECK(mgr.any_above_normal());
}

int main() { return t::run_all(); }
