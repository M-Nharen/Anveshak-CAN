// End-to-end gateway behaviour on the in-process fake bus: real threads, real 20 ms cycle.
// Timing bounds are deliberately loose upper limits so a busy CI machine does not flake; the
// lower bounds (what must NOT happen early) are exact.
#include <dirent.h>
#include <unistd.h>

#include <cmath>
#include <cstdlib>

#include "anv_gateway/gateway_link.hpp"
#include "anv_gateway/status.hpp"
#include "rover.h"
#include "test_util.hpp"

using namespace anv::gw;
using namespace t;

namespace {

constexpr int64_t kMs = kNsPerMs;

// A laptop at the service port: injects LEAD_REQUEST frames the way the dashboard or the tool would.
class Tool {
 public:
  explicit Tool(FakeBusHub& hub) : port_(hub.connect()) {}
  void request_lead(uint8_t host) { port_->send(make_lead_request({kNodeTool, host, ++counter_})); }
  void send(const CanFrame& f) { port_->send(f); }

 private:
  std::unique_ptr<ICanBus> port_;
  uint8_t counter_ = 0;
};

struct Host {
  std::unique_ptr<Gateway> gw;
  std::vector<GatewayEvent> events;
  std::mutex mu;

  Host(FakeBusHub& hub, uint8_t id, bool auto_takeover = false, GatewayLink* link = nullptr,
       const std::function<void(GatewayConfig&)>& configure = nullptr)
      : gw(make_gateway(hub, id, auto_takeover, configure)) {
    gw->subscribe([this](const GatewayEvent& e) {
      std::lock_guard<std::mutex> lk(mu);
      events.push_back(e);
    });
    if (link) gw->attach_link(link);
    gw->start();
  }

  static std::unique_ptr<Gateway> make_gateway(FakeBusHub& hub, uint8_t id, bool auto_takeover,
                                                const std::function<void(GatewayConfig&)>& configure) {
    GatewayConfig cfg = host_config(id, auto_takeover);
    if (configure) configure(cfg);
    return std::make_unique<Gateway>(cfg, hub.connect());
  }
  // Stop the threads before the members the event callback touches are destroyed.
  ~Host() { gw->stop(); }
  std::vector<GatewayEvent> copy_events() {
    std::lock_guard<std::mutex> lk(mu);
    return events;
  }
  bool saw(EventType type, uint8_t node_id = 0xFF) {
    for (const auto& e : copy_events())
      if (e.type == type && (node_id == 0xFF || e.node_id == node_id)) return true;
    return false;
  }
  GatewayEvent first(EventType type, uint8_t node_id = 0xFF) {
    for (const auto& e : copy_events())
      if (e.type == type && (node_id == 0xFF || e.node_id == node_id)) return e;
    return {};
  }
  Role role() const { return gw->state().role; }
  uint16_t term() const { return gw->state().term; }
};

bool is_drive_cmd_id(uint16_t id) { return (id >> 8) == 1; }

}  // namespace

TEST(with_two_gateways_and_no_request_nobody_leads_and_no_commands_are_sent) {
  FakeBusHub hub;
  Recorder rec(hub);
  Host orin(hub, 1), rpi(hub, 2);
  sleep_ms(600);

  CHECK(orin.role() == Role::Standby);
  CHECK(rpi.role() == Role::Standby);
  CHECK_EQ(orin.term(), 0);
  CHECK_EQ(rpi.term(), 0);
  CHECK_EQ(orin.gw->state().believed_master, 0);

  for (const Decoded& d : rec.all()) {
    CHECK(d.id != 0x002);          // no SYNC
    CHECK(!is_drive_cmd_id(d.id)); // no DRIVE_CMD
    CHECK(d.id != 0x003);          // no NMT
  }
  // Heartbeats at 20 Hz each: about 12 in 600 ms; accept a wide band.
  const size_t orin_hb = rec.count_id(0x008), rpi_hb = rec.count_id(0x010);
  CHECK(orin_hb >= 8 && orin_hb <= 16);
  CHECK(rpi_hb >= 8 && rpi_hb <= 16);
  for (auto& [ts, hb] : rec.of<HostHbMsg>()) CHECK_EQ(hb.role, static_cast<uint8_t>(Role::Standby));
}

TEST(lead_requests_hand_the_lead_back_and_forth_with_a_rising_term) {
  FakeBusHub hub;
  Recorder rec(hub);
  Tool tool(hub);
  Host orin(hub, 1), rpi(hub, 2);
  sleep_ms(150);

  tool.request_lead(2);
  CHECK(wait_until([&] { return rpi.role() == Role::Master; }, 400));
  CHECK_EQ(rpi.term(), 1);
  CHECK(wait_until([&] { return orin.gw->state().believed_master == 2; }, 400));
  CHECK(orin.role() == Role::Standby);
  sleep_ms(150);  // let the new master run a few cycles

  tool.request_lead(1);
  CHECK(wait_until([&] { return orin.role() == Role::Master; }, 400));
  CHECK_EQ(orin.term(), 2);
  CHECK(wait_until([&] { return rpi.role() == Role::Standby; }, 400));
  CHECK_EQ(rpi.term(), 2);
  CHECK_EQ(rpi.gw->state().believed_master, 1);
  sleep_ms(150);

  // Every command carries its sender and the low bits of its term, and they always agree.
  const auto cmds = rec.of<DriveCmdMsg>();
  CHECK(!cmds.empty());
  size_t from_rpi = 0, from_orin = 0;
  int64_t last_rpi_cmd = 0, first_orin_cmd = INT64_MAX;
  for (const auto& [ts, c] : cmds) {
    if (c.sender_host == 2) { CHECK_EQ(c.term_low, 1); ++from_rpi; last_rpi_cmd = std::max(last_rpi_cmd, ts); }
    if (c.sender_host == 1) { CHECK_EQ(c.term_low, 2); ++from_orin; first_orin_cmd = std::min(first_orin_cmd, ts); }
  }
  CHECK(from_rpi >= 12 && from_orin >= 12);  // six commands per cycle, a few cycles each

  // Only one host sends commands at any moment: the RPi's last command precedes the Orin's
  // claim heartbeat + one cycle (it steps down the moment it hears the higher term).
  int64_t orin_claim_hb = INT64_MAX;
  for (const auto& [ts, hb] : rec.of<HostHbMsg>())
    if (hb.host_node == 1 && hb.role == static_cast<uint8_t>(Role::Master)) orin_claim_hb = std::min(orin_claim_hb, ts);
  CHECK(orin_claim_hb != INT64_MAX);
  CHECK(last_rpi_cmd <= orin_claim_hb + 60 * kMs);
  CHECK(first_orin_cmd >= orin_claim_hb - 5 * kMs);  // the claim is announced before commands start

  // Terms announced in master heartbeats only ever rise.
  uint16_t prev = 0;
  for (const auto& [ts, hb] : rec.of<HostHbMsg>()) {
    if (hb.role != static_cast<uint8_t>(Role::Master)) continue;
    CHECK(hb.term >= prev);
    prev = hb.term;
  }
}

TEST(a_claim_is_announced_in_a_heartbeat_within_50_ms) {
  FakeBusHub hub;
  Recorder rec(hub);
  Tool tool(hub);
  Host rpi(hub, 2);
  sleep_ms(100);
  const int64_t t0 = mono_ns();
  tool.request_lead(2);
  CHECK(wait_until([&] {
    for (auto& [ts, hb] : rec.of<HostHbMsg>())
      if (hb.role == static_cast<uint8_t>(Role::Master) && ts >= t0) return true;
    return false;
  }, 300));
  int64_t announced = 0;
  for (auto& [ts, hb] : rec.of<HostHbMsg>())
    if (hb.role == static_cast<uint8_t>(Role::Master)) { announced = ts; break; }
  CHECK(announced - t0 <= 50 * kMs + 20 * kMs);  // 50 ms rule, plus scheduling slack
  CHECK_EQ(rec.of<HostHbMsg>().back().second.term, 1);
}

TEST(killing_the_leader_raises_leader_lost_at_250_ms_and_nothing_takes_over_by_default) {
  FakeBusHub hub;
  Recorder rec(hub);
  Tool tool(hub);
  Host orin(hub, 1), rpi(hub, 2);
  sleep_ms(100);
  tool.request_lead(1);
  CHECK(wait_until([&] { return orin.role() == Role::Master; }, 400));
  CHECK(wait_until([&] { return rpi.gw->state().believed_master == 1; }, 400));
  sleep_ms(200);

  orin.gw->stop();  // the Orin "crashes": it says nothing more
  sleep_ms(30);
  int64_t last_master_frame = 0;
  for (auto& [ts, hb] : rec.of<HostHbMsg>())
    if (hb.host_node == 1) last_master_frame = std::max(last_master_frame, ts);
  const size_t frames_at_kill = rec.all().size();

  CHECK(wait_until([&] { return rpi.saw(EventType::LeaderLost); }, 900));
  const GatewayEvent lost = rpi.first(EventType::LeaderLost);
  const int64_t dt = lost.time_ns - last_master_frame;
  CHECK(dt >= 250 * kMs - kMs);   // never early
  CHECK(dt <= 450 * kMs);         // about 250 ms, with generous scheduling slack
  CHECK_EQ(lost.node_id, 1);
  CHECK(rpi.gw->state().leader_lost);

  sleep_ms(500);
  CHECK(rpi.role() == Role::Standby);  // auto_takeover is off: the operator decides
  const std::vector<Decoded> after = rec.all();
  for (size_t i = frames_at_kill; i < after.size(); ++i)
    CHECK(after[i].id != 0x002 && !is_drive_cmd_id(after[i].id));  // no SYNC or commands from anyone
}

TEST(with_auto_takeover_the_standby_claims_after_the_leader_vanishes) {
  FakeBusHub hub;
  Recorder rec(hub);
  Tool tool(hub);
  Host orin(hub, 1), rpi(hub, 2, /*auto_takeover=*/true);
  sleep_ms(100);
  tool.request_lead(1);
  CHECK(wait_until([&] { return orin.role() == Role::Master; }, 400));
  CHECK(wait_until([&] { return rpi.gw->state().believed_master == 1; }, 400));
  sleep_ms(200);

  orin.gw->stop();
  sleep_ms(30);
  int64_t last_master_frame = 0;
  for (auto& [ts, hb] : rec.of<HostHbMsg>())
    if (hb.host_node == 1) last_master_frame = std::max(last_master_frame, ts);

  CHECK(wait_until([&] { return rpi.role() == Role::Master; }, 900));
  // Events are delivered just after the state changes, so wait for them instead of sampling once.
  CHECK(wait_until([&] { return rpi.saw(EventType::LeaderLost) && rpi.saw(EventType::RoleChanged); }, 300));
  const GatewayEvent claimed = rpi.first(EventType::RoleChanged);
  CHECK(claimed.time_ns - last_master_frame >= 250 * kMs - kMs);
  CHECK(claimed.time_ns - last_master_frame <= 450 * kMs);
  CHECK_EQ(rpi.term(), 2);
  CHECK(wait_until([&] { return rec.count_id(0x002) > 0 && rec.of<DriveCmdMsg>().size() > 6; }, 300));
  // Nodes would still be disarmed: the takeover sends no ARM.
  const std::vector<Decoded> everything = rec.all();
  for (const Decoded& d : everything) CHECK(d.id != 0x003);
}

TEST(stale_targets_become_zero_speed_commands_after_100_ms) {
  FakeBusHub hub;
  Recorder rec(hub);
  Tool tool(hub);
  Host orin(hub, 1);
  sleep_ms(50);
  tool.request_lead(1);
  CHECK(wait_until([&] { return orin.role() == Role::Master; }, 400));

  WheelTarget w;
  w.a = 1.5;
  w.b = -2.0;
  std::array<WheelTarget, kNumDrives> all;
  all.fill(w);
  int64_t last_set = 0;
  for (int i = 0; i < 15; ++i) {  // refresh for 300 ms, as autonomy or teleop would
    orin.gw->set_targets(all);
    last_set = mono_ns();
    sleep_ms(20);
  }
  sleep_ms(400);  // then go quiet

  size_t fresh_nonzero = 0, late_zero = 0;
  for (auto& [ts, c] : rec.of<DriveCmdMsg>()) {
    if (c.drive != 1) continue;
    const bool zero = c.target_a == 0 && c.target_b == 0;
    if (ts <= last_set + 100 * kMs && ts > last_set - 200 * kMs && !zero) {
      CHECK_EQ(c.target_a, 150);   // 1.5 rad/s at 0.01 rad/s per count
      CHECK_EQ(c.target_b, -200);
      CHECK_EQ(c.mode, static_cast<uint8_t>(DriveMode::Velocity));
      ++fresh_nonzero;
    }
    if (ts > last_set + 125 * kMs) {
      CHECK(zero);  // 100 ms stale + one cycle of slack: never a non-zero command
      CHECK_EQ(c.mode, static_cast<uint8_t>(DriveMode::Velocity));
      ++late_zero;
    }
    if (ts > last_set + 105 * kMs && ts <= last_set + 125 * kMs) { /* inside the cycle that crosses 100 ms */ }
  }
  CHECK(fresh_nonzero >= 5);
  CHECK(late_zero >= 10);  // commands keep flowing (zeros), so node watchdogs stay fed
}

TEST(targets_for_each_drive_land_on_that_drives_command_id) {
  FakeBusHub hub;
  Recorder rec(hub);
  Tool tool(hub);
  Host orin(hub, 1);
  tool.request_lead(1);
  CHECK(wait_until([&] { return orin.role() == Role::Master; }, 400));
  for (uint8_t d = 1; d <= 6; ++d) {
    WheelTarget w;
    w.a = d;
    w.b = -d;
    CHECK(orin.gw->set_wheel_target(d, w) == CmdResult::Ok);
  }
  CHECK(orin.gw->set_wheel_target(7, WheelTarget{}) == CmdResult::BadArgument);
  sleep_ms(60);
  std::array<bool, 7> seen{};
  for (auto& [ts, c] : rec.of<DriveCmdMsg>())
    if (c.target_a == c.drive * 100 && c.target_b == -c.drive * 100) seen[c.drive] = true;
  for (uint8_t d = 1; d <= 6; ++d) CHECK(seen[d]);
  // SYNC precedes the six commands of each cycle.
  const auto all = rec.all();
  for (size_t i = 0; i + 6 < all.size(); ++i) {
    if (all[i].id == 0x002) {
      size_t cmds = 0;
      for (size_t k = i + 1; k < all.size() && k <= i + 6; ++k) cmds += is_drive_cmd_id(all[k].id);
      if (i + 6 < all.size() - 7) CHECK_EQ(cmds, 6u);
    }
  }
}

TEST(registry_flags_a_missing_node_within_300_ms_and_a_reboot_by_its_uptime) {
  FakeBusHub hub;
  Host orin(hub, 1);
  FakeNode node(hub, drive_node_id(1));
  node.set_uptime(100);

  auto drive1 = [&] {
    const GatewayState st = orin.gw->state();
    for (const auto& n : st.nodes)
      if (n.node_id == 4) return n;
    return NodeInfo{};
  };
  CHECK(wait_until([&] { return drive1().alive; }, 500));
  CHECK(orin.saw(EventType::NodeAlive, 4));
  CHECK_EQ(drive1().uptime_s, 100);
  CHECK(drive1().seen);
  // The other nodes never spoke.
  const GatewayState others = orin.gw->state();
  for (const auto& n : others.nodes)
    if (n.node_id != 4) CHECK(!n.seen && !n.alive);

  node.set_sending(false);
  sleep_ms(5);
  const int64_t last = node.last_send_ns();
  CHECK(wait_until([&] { return orin.saw(EventType::NodeMissing, 4); }, 900));
  const int64_t dt = orin.first(EventType::NodeMissing, 4).time_ns - last;
  CHECK(dt >= 300 * kMs - kMs);
  CHECK(dt <= 450 * kMs);
  CHECK(!drive1().alive);

  node.set_uptime(3);  // it comes back having rebooted
  node.set_sending(true);
  CHECK(wait_until([&] { return orin.saw(EventType::NodeRebooted, 4); }, 900));
  CHECK(wait_until([&] { return drive1().alive; }, 500));
  CHECK_EQ(drive1().reboot_count, 1u);
}

TEST(arm_disarm_and_estop_follow_the_master_only_rules) {
  FakeBusHub hub;
  Recorder rec(hub);
  Tool tool(hub);
  Host orin(hub, 1), rpi(hub, 2);
  sleep_ms(100);

  // A standby host can't arm or disarm, but any host can E-stop.
  CHECK(orin.gw->arm() == CmdResult::NotMaster);
  CHECK(orin.gw->disarm() == CmdResult::NotMaster);
  CHECK(orin.gw->release_estop() == CmdResult::NotMaster);
  CHECK(rpi.gw->estop() == CmdResult::Ok);
  CHECK(wait_until([&] { return orin.gw->state().estop_latched && rpi.gw->state().estop_latched; }, 300));
  CHECK(orin.saw(EventType::EstopSeen));
  CHECK(wait_until([&] { return rec.raw_with_id(0x000).size() == 1; }, 300));
  const auto estop_frames = rec.raw_with_id(0x000);
  CHECK_EQ(estop_frames.size(), 1u);
  if (!estop_frames.empty()) CHECK_EQ(estop_frames[0].data[0], 0xE5);

  tool.request_lead(1);
  CHECK(wait_until([&] { return orin.role() == Role::Master; }, 400));
  CHECK(orin.gw->arm() == CmdResult::EstopLatched);  // a latched E-stop blocks ARM
  sleep_ms(80);
  for (auto& [ts, c] : rec.of<DriveCmdMsg>()) CHECK(c.target_a == 0 && c.target_b == 0);

  CHECK(orin.gw->release_estop() == CmdResult::Ok);
  CHECK(wait_until([&] { return !rpi.gw->state().estop_latched; }, 300));
  CHECK(wait_until([&] { return rec.raw_with_id(0x001).size() == 1; }, 300));
  const auto rel = rec.raw_with_id(0x001);
  CHECK_EQ(rel.size(), 1u);
  if (!rel.empty()) {
    CHECK_EQ(rel[0].data[0], 1);  // master node ID
    CHECK_EQ(rel[0].data[1], 1);  // term
  }

  CHECK(orin.gw->arm() == CmdResult::Ok);
  CHECK(orin.gw->state().armed);
  CHECK(wait_until([&] { return rec.of<NmtMsg>().size() == 1; }, 300));
  const auto nmt = rec.of<NmtMsg>();
  CHECK_EQ(nmt.size(), 1u);
  if (!nmt.empty()) {
    CHECK_EQ(nmt[0].second.target_node, 0);  // all nodes
    CHECK_EQ(nmt[0].second.command, static_cast<uint8_t>(NmtCommand::Arm));
    CHECK_EQ(nmt[0].second.master_node_id, 1);
    CHECK_EQ(nmt[0].second.master_term, 1);
  }
  CHECK(wait_until([&] {
    for (auto& [ts, hb] : rec.of<HostHbMsg>())
      if (hb.host_node == 1 && (hb.flags & kHbFlagArmed)) return true;
    return false;
  }, 300));

  // A handover invalidates the arming: the new master must ARM again, like the nodes do.
  tool.request_lead(2);
  CHECK(wait_until([&] { return rpi.role() == Role::Master; }, 400));
  CHECK(wait_until([&] { return orin.role() == Role::Standby; }, 400));
  CHECK(!orin.gw->state().armed);
  CHECK(!rpi.gw->state().armed);
  CHECK(rpi.gw->arm() == CmdResult::Ok);
  CHECK(rpi.gw->disarm() == CmdResult::Ok);
  CHECK(!rpi.gw->state().armed);
}

TEST(bad_arguments_and_use_after_stop_are_reported_not_thrown) {
  FakeBusHub hub;
  Host orin(hub, 1);
  CHECK(orin.gw->request_lead(0) == CmdResult::BadArgument);
  CHECK(orin.gw->request_lead(3) == CmdResult::BadArgument);
  orin.gw->stop();
  CHECK(!orin.gw->running());
  CHECK(orin.gw->request_lead(1) == CmdResult::NotRunning);
  CHECK(orin.gw->estop() == CmdResult::NotRunning);
  orin.gw->stop();  // idempotent
}

TEST(a_local_lead_request_makes_this_host_lead_and_tells_the_other) {
  FakeBusHub hub;
  Host orin(hub, 1), rpi(hub, 2);
  sleep_ms(100);
  CHECK(rpi.gw->request_lead(2) == CmdResult::Ok);  // "make me lead" from the RPi's own dashboard
  CHECK(wait_until([&] { return rpi.role() == Role::Master; }, 400));
  CHECK(wait_until([&] { return orin.gw->state().believed_master == 2; }, 400));
  CHECK(orin.gw->request_lead(1) == CmdResult::Ok);  // ... and hand it back from the Orin's
  CHECK(wait_until([&] { return orin.role() == Role::Master && rpi.role() == Role::Standby; }, 600));
  CHECK_EQ(orin.term(), 2);
}

TEST(status_text_lists_every_node_and_the_role) {
  FakeBusHub hub;
  Host orin(hub, 1);
  FakeNode node(hub, drive_node_id(2));
  CHECK(wait_until([&] {
    const GatewayState st = orin.gw->state();
    for (const auto& n : st.nodes)
      if (n.node_id == 5) return n.alive;
    return false;
  }, 500));
  const std::string s = format_status(orin.gw->state());
  CHECK(s.find("STANDBY") != std::string::npos);
  CHECK(s.find("DRIVE2") != std::string::npos);
  CHECK(s.find("alive") != std::string::npos);
  CHECK(s.find("never seen") != std::string::npos);  // the other six
  CHECK(s.find("FPGA") != std::string::npos);
}

// ---------------------------------------------------------------------------------------------
// B4: the gateway link, debug levels, the event stream and the flight recorder
// ---------------------------------------------------------------------------------------------

TEST(a_standby_hosts_local_targets_reach_the_leader_over_the_gateway_link) {
  FakeBusHub hub;
  Recorder rec(hub);

  GatewayLink orin_link({23601, "127.0.0.1", 23602});
  GatewayLink rpi_link({23602, "127.0.0.1", 23601});
  std::string err;
  CHECK(orin_link.start(&err));
  CHECK(rpi_link.start(&err));

  Host orin(hub, 1, false, &orin_link), rpi(hub, 2, false, &rpi_link);
  sleep_ms(100);
  CHECK(orin.gw->request_lead(1) == CmdResult::Ok);
  CHECK(wait_until([&] { return orin.role() == Role::Master; }, 400));

  // The RPi is standby and never asked to lead, but its local dashboard/autonomy can still set
  // targets; they must reach the Orin over the link and come out as DRIVE_CMD on the bus.
  WheelTarget t;
  t.mode = DriveMode::Velocity;
  t.a = 1.23;
  t.b = -1.23;
  std::array<WheelTarget, kNumDrives> all;
  all.fill(t);
  rpi.gw->set_targets(all);

  CHECK(wait_until(
      [&] {
        for (auto& [ts, c] : rec.of<DriveCmdMsg>())
          if (c.sender_host == 1 && std::abs(speed_rad_s(c.target_a) - 1.23) < 0.02) return true;
        return false;
      },
      1000));

  orin_link.stop();
  rpi_link.stop();
}

TEST(a_quiet_gateway_link_is_reported_lost_after_1_s) {
  FakeBusHub hub;
  auto orin_link = std::make_unique<GatewayLink>(GatewayLinkConfig{23611, "127.0.0.1", 23612});
  GatewayLink rpi_link({23612, "127.0.0.1", 23611});
  std::string err;
  CHECK(orin_link->start(&err));
  CHECK(rpi_link.start(&err));

  Host orin(hub, 1, false, orin_link.get()), rpi(hub, 2, false, &rpi_link);
  CHECK(wait_until([&] { return rpi_link.peer_up(mono_ns()); }, 500));

  orin_link->stop();  // the Orin's link goes quiet; its gateway keeps running
  CHECK(wait_until([&] { return rpi.saw(EventType::GatewayLinkLost); }, 2000));

  rpi_link.stop();
}

TEST(a_master_only_gateway_set_debug_level_puts_debug_level_on_the_bus) {
  FakeBusHub hub;
  Recorder rec(hub);
  Host orin(hub, 1);
  CHECK(orin.gw->request_lead(1) == CmdResult::Ok);
  CHECK(wait_until([&] { return orin.role() == Role::Master; }, 400));

  CHECK(orin.gw->set_debug_level(0, 1) == CmdResult::Ok);
  CHECK(wait_until([&] { return rec.count_id(0x004) >= 1; }, 400));
  CHECK(orin.gw->set_debug_level(99, 1) == CmdResult::BadArgument);  // node id out of range
  CHECK(orin.gw->set_debug_level(0, 9) == CmdResult::BadArgument);   // level out of range
}

TEST(a_node_event_sequence_gap_triggers_a_log_read_request) {
  FakeBusHub hub;
  Recorder rec(hub);
  Host orin(hub, 1);
  auto port = hub.connect();

  NodeEventMsg e1;
  e1.node_id = drive_node_id(2);  // node 5
  e1.severity = 1;
  e1.error_code = 0x0304;
  e1.event_seq = 10;
  port->send(make_node_event(e1));
  sleep_ms(50);
  CHECK(rec.raw_with_id(ROVER_DRIVE2_SERVICE_REQ_ORIN_FRAME_ID).empty());  // no gap yet

  NodeEventMsg e2 = e1;
  e2.event_seq = 13;  // skipped 11 and 12
  port->send(make_node_event(e2));

  CHECK(wait_until([&] { return orin.saw(EventType::EventGapDetected, 5); }, 500));
  CHECK(wait_until([&] { return !rec.raw_with_id(ROVER_DRIVE2_SERVICE_REQ_ORIN_FRAME_ID).empty(); }, 500));
  const auto reqs = rec.raw_with_id(ROVER_DRIVE2_SERVICE_REQ_ORIN_FRAME_ID);
  CHECK(!reqs.empty());
  CHECK_EQ(reqs.back().data[0], kServiceOpLogRead);  // opcode 6, byte 0 of the request
}

TEST(a_critical_node_event_saves_a_flight_recording) {
  char dir[] = "/tmp/anv_flight_gw_test_XXXXXX";
  CHECK(::mkdtemp(dir) != nullptr);

  FakeBusHub hub;
  Host orin(hub, 1, false, nullptr, [&](GatewayConfig& cfg) {
    cfg.flight_recorder = true;
    cfg.flight_recorder_dir = dir;
  });
  auto port = hub.connect();

  NodeEventMsg crit;
  crit.node_id = drive_node_id(1);
  crit.severity = 3;  // critical
  crit.error_code = 0x0602;  // HARD_FAULT
  crit.event_seq = 1;
  port->send(make_node_event(crit));

  CHECK(wait_until([&] { return orin.saw(EventType::FlightRecorderSaved); }, 500));

  DIR* d = ::opendir(dir);
  CHECK(d != nullptr);
  bool found = false;
  if (d) {
    for (dirent* ent = ::readdir(d); ent != nullptr; ent = ::readdir(d)) {
      const std::string name = ent->d_name;
      if (name.find("flight_") == 0) found = true;
    }
    ::closedir(d);
  }
  CHECK(found);

  // Clean up: remove every file we created, then the directory.
  if ((d = ::opendir(dir))) {
    for (dirent* ent = ::readdir(d); ent != nullptr; ent = ::readdir(d)) {
      const std::string name = ent->d_name;
      if (name != "." && name != "..") std::remove((std::string(dir) + "/" + name).c_str());
    }
    ::closedir(d);
  }
  ::rmdir(dir);
}

int main() { return t::run_all(); }
