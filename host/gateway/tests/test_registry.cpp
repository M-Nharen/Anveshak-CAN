// Node registry with a fake clock: liveness, reboot detection, data and rate tracking.
#include "anv_gateway/node_registry.hpp"
#include "test_util.hpp"

using namespace anv::gw;

namespace {

constexpr int64_t kMs = kNsPerMs;

Decoded hb(uint8_t node, uint16_t uptime, int64_t at, uint8_t debug_level = 0) {
  NodeHbMsg m;
  m.node_id = node;
  m.state = 1;
  m.node_type = 1;
  m.protocol_version = 1;
  m.debug_level = debug_level;
  m.fw_build_id = 0xABCD;
  m.uptime_s = uptime;
  Decoded d;
  CHECK(decode_frame(make_node_hb(m), d));
  d.rx_ns = at;
  return d;
}

Decoded fb_a(uint8_t drive, int64_t at) {
  DriveFbAMsg m;
  m.drive = drive;
  m.state = 2;
  m.speed_a = 123;
  Decoded d;
  CHECK(decode_frame(make_drive_fb_a(m), d));
  d.rx_ns = at;
  return d;
}

size_t count(const std::vector<GatewayEvent>& ev, EventType t, uint8_t node) {
  size_t n = 0;
  for (const auto& e : ev) n += e.type == t && e.node_id == node;
  return n;
}

const NodeInfo* find(const std::vector<NodeInfo>& v, uint8_t node) {
  for (const auto& n : v)
    if (n.node_id == node) return &n;
  return nullptr;
}

}  // namespace

TEST(starts_with_the_rover_nodes_never_seen) {
  NodeRegistry r;
  const auto snap = r.snapshot(0);
  CHECK_EQ(snap.size(), 7u);  // drive 1-6 + FPGA
  for (const auto& n : snap) CHECK(!n.seen && !n.alive);
  CHECK(find(snap, 20) != nullptr && find(snap, 20)->name == "FPGA");
  CHECK(find(snap, 4)->name == "DRIVE1");
}

TEST(a_node_is_missing_300_ms_after_its_last_frame) {
  NodeRegistry r;
  std::vector<GatewayEvent> ev;
  r.on_decoded(hb(4, 10, 1000 * kMs), ev);
  CHECK_EQ(count(ev, EventType::NodeAlive, 4), 1u);
  CHECK(find(r.snapshot(1000 * kMs), 4)->alive);

  r.tick(1299 * kMs, ev);
  CHECK_EQ(count(ev, EventType::NodeMissing, 4), 0u);
  CHECK(find(r.snapshot(1299 * kMs), 4)->alive);

  r.tick(1301 * kMs, ev);
  CHECK_EQ(count(ev, EventType::NodeMissing, 4), 1u);
  CHECK(!find(r.snapshot(1301 * kMs), 4)->alive);
  CHECK(find(r.snapshot(1301 * kMs), 4)->seen);  // missing, not "never seen"

  r.tick(2000 * kMs, ev);
  CHECK_EQ(count(ev, EventType::NodeMissing, 4), 1u);  // once, not every tick

  r.on_decoded(hb(4, 12, 2100 * kMs), ev);
  CHECK_EQ(count(ev, EventType::NodeAlive, 4), 2u);  // came back
  CHECK_EQ(count(ev, EventType::NodeRebooted, 4), 0u);
}

TEST(snapshot_does_not_report_a_stale_node_alive_even_without_a_tick) {
  NodeRegistry r;
  std::vector<GatewayEvent> ev;
  r.on_decoded(hb(5, 1, 0), ev);
  CHECK(!find(r.snapshot(400 * kMs), 5)->alive);
}

TEST(any_node_frame_counts_as_a_sign_of_life) {
  NodeRegistry r;
  std::vector<GatewayEvent> ev;
  r.on_decoded(hb(6, 1, 0), ev);
  for (int64_t t = 20 * kMs; t < 1000 * kMs; t += 20 * kMs) {  // only 50 Hz feedback, no more heartbeats
    r.on_decoded(fb_a(3, t), ev);
    r.tick(t, ev);
  }
  CHECK_EQ(count(ev, EventType::NodeMissing, 6), 0u);
  const NodeInfo* n = find(r.snapshot(980 * kMs), 6);
  CHECK(n->has_fb_a);
  CHECK_EQ(n->fb_a.speed_a, 123);
  CHECK_EQ(n->state, 2);
}

TEST(a_reboot_is_an_uptime_that_goes_backwards) {
  NodeRegistry r;
  std::vector<GatewayEvent> ev;
  r.on_decoded(hb(7, 100, 0), ev);
  r.on_decoded(hb(7, 101, 100 * kMs), ev);
  CHECK_EQ(count(ev, EventType::NodeRebooted, 7), 0u);
  r.on_decoded(hb(7, 2, 200 * kMs), ev);
  CHECK_EQ(count(ev, EventType::NodeRebooted, 7), 1u);
  const NodeInfo* n = find(r.snapshot(200 * kMs), 7);
  CHECK_EQ(n->reboot_count, 1u);
  CHECK_EQ(n->uptime_s, 2);
  CHECK_EQ(n->fw_build_id, 0xABCD);
}

TEST(uptime_wrap_is_not_a_reboot) {
  NodeRegistry r;
  std::vector<GatewayEvent> ev;
  r.on_decoded(hb(8, 65535, 0), ev);
  r.on_decoded(hb(8, 0, 100 * kMs), ev);
  CHECK_EQ(count(ev, EventType::NodeRebooted, 8), 0u);
}

TEST(first_heartbeat_is_never_a_reboot) {
  NodeRegistry r;
  std::vector<GatewayEvent> ev;
  r.on_decoded(hb(9, 3, 0), ev);  // low uptime on first sight: node just booted, nothing to compare to
  CHECK_EQ(count(ev, EventType::NodeRebooted, 9), 0u);
}

TEST(measured_rate_is_compared_with_the_expected_rate) {
  NodeRegistry r;
  std::vector<GatewayEvent> ev;
  // Drive 1 feedback at 50 Hz for 2 s, drive 2 at 25 Hz (a node dropping frames).
  for (int64_t t = 0; t <= 2000 * kMs; t += 20 * kMs) {
    r.on_decoded(fb_a(1, t), ev);
    if ((t / (20 * kMs)) % 2 == 0) r.on_decoded(fb_a(2, t), ev);
    r.tick(t, ev);
  }
  double d1 = -1, d2 = -1;
  bool low1 = true, low2 = false;
  for (const RateInfo& ri : r.rates()) {
    if (ri.id == 0x220) { d1 = ri.measured_hz; low1 = ri.low; CHECK(ri.expected_hz == 50.0); }
    if (ri.id == 0x228) { d2 = ri.measured_hz; low2 = ri.low; }
  }
  CHECK(d1 > 47.0 && d1 < 53.0);
  CHECK(d2 > 22.0 && d2 < 28.0);
  CHECK(!low1);
  CHECK(low2);
}

TEST(a_silent_frame_id_decays_to_zero_hz) {
  NodeRegistry r;
  std::vector<GatewayEvent> ev;
  for (int64_t t = 0; t < 1000 * kMs; t += 20 * kMs) r.on_decoded(fb_a(1, t), ev);
  r.tick(1000 * kMs, ev);
  r.tick(2100 * kMs, ev);  // a whole window with no frames
  for (const RateInfo& ri : r.rates())
    if (ri.id == 0x220) CHECK(ri.measured_hz == 0.0 && ri.low);
}

TEST(expected_rates_follow_the_debug_level) {
  CHECK(NodeRegistry::expected_hz(0x320, 0) == 1.0);   // telemetry: 1 Hz at level 0
  CHECK(NodeRegistry::expected_hz(0x320, 1) == 10.0);
  CHECK(NodeRegistry::expected_hz(0x322, 0) == 0.0);   // debug frames only at level 2 / 3
  CHECK(NodeRegistry::expected_hz(0x322, 2) == 10.0);
  CHECK(NodeRegistry::expected_hz(0x322, 3) == 50.0);
  CHECK(NodeRegistry::expected_hz(0x620, 0) == 10.0);  // node heartbeat
  CHECK(NodeRegistry::expected_hz(0x327, 0) == 0.0);   // events are not periodic
  CHECK(NodeRegistry::expected_hz(0x4A0, 0) == 50.0);
}

TEST(telemetry_and_sensor_values_are_kept) {
  NodeRegistry r;
  std::vector<GatewayEvent> ev;
  DriveTelem0Msg t0;
  t0.drive = 1; t0.tec = 9; t0.rec = 4; t0.supply_voltage_mv = 24000;
  Decoded d;
  d.id = 0x320; d.rx_ns = 0; d.msg = t0;
  r.on_decoded(d, ev);
  FpgaSensorsMsg s;
  s.second_half = true; s.values = {10, 20, 30, 40};
  d.id = 0x4A1; d.msg = s;
  r.on_decoded(d, ev);
  const auto snap = r.snapshot(0);
  CHECK_EQ(find(snap, 4)->tec, 9);
  CHECK_EQ(find(snap, 4)->rec, 4);
  CHECK_EQ(find(snap, 20)->sensors[7], 40);
  CHECK(find(snap, 20)->has_sensors);
}

int main() { return t::run_all(); }
