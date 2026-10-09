// The election rules from docs/architecture.md "Hosts: choosing which one leads", with a fake clock.
#include "anv_gateway/leadership.hpp"
#include "test_util.hpp"

using namespace anv::gw;

namespace {

constexpr int64_t kMs = kNsPerMs;

HostHbMsg peer_hb(uint8_t host, Role role, uint16_t term, uint8_t believed = 0) {
  HostHbMsg hb;
  hb.host_node = host;
  hb.role = static_cast<uint8_t>(role);
  hb.tie_break = host;
  hb.term = term;
  hb.believed_master = believed;
  return hb;
}

LeadRequestMsg request(uint8_t sender, uint8_t leader, uint8_t counter) { return {sender, leader, counter}; }

ElectionConfig cfg(uint8_t host, bool auto_takeover = false) {
  ElectionConfig c;
  c.host_id = host;
  c.tie_break = host;
  c.auto_takeover = auto_takeover;
  return c;
}

size_t count(const std::vector<GatewayEvent>& ev, EventType t) {
  size_t n = 0;
  for (const auto& e : ev) n += e.type == t;
  return n;
}

}  // namespace

TEST(boots_as_standby_and_never_claims_by_itself) {
  LeaderElection e(cfg(1));
  CHECK(e.role() == Role::Booting);
  std::vector<GatewayEvent> ev;
  e.start(0);
  CHECK(e.role() == Role::Standby);
  // Time passing, peers chattering as standby: still nothing.
  for (int64_t t = 0; t < 5000 * kMs; t += 10 * kMs) {
    e.on_host_hb(peer_hb(2, Role::Standby, 0), t, ev);
    e.tick(t, ev);
  }
  CHECK(e.role() == Role::Standby);
  CHECK_EQ(e.term(), 0);
  CHECK(ev.empty());
}

TEST(claims_only_when_a_request_names_this_host) {
  LeaderElection e(cfg(2));
  std::vector<GatewayEvent> ev;
  e.start(0);
  e.on_lead_request(request(3, 1, 1), 10 * kMs, ev);  // names the Orin: not us
  CHECK(e.role() == Role::Standby);
  e.on_lead_request(request(3, 2, 2), 20 * kMs, ev);  // names us
  CHECK(e.role() == Role::Master);
  CHECK_EQ(e.term(), 1);
  CHECK_EQ(e.believed_master(), 2);
  CHECK_EQ(count(ev, EventType::RoleChanged), 1u);
  CHECK(e.take_hb_urgent());   // claim must be announced at once
  CHECK(!e.take_hb_urgent());  // ... once
}

TEST(claim_term_is_highest_seen_plus_one) {
  LeaderElection e(cfg(1));
  std::vector<GatewayEvent> ev;
  e.start(0);
  e.on_host_hb(peer_hb(2, Role::Standby, 5), 10 * kMs, ev);  // the peer has seen term 5
  e.on_lead_request(request(3, 1, 1), 20 * kMs, ev);
  CHECK_EQ(e.term(), 6);
}

TEST(repeat_request_for_current_leader_does_not_bump_the_term) {
  LeaderElection e(cfg(1));
  std::vector<GatewayEvent> ev;
  e.start(0);
  e.on_lead_request(request(3, 1, 1), 0, ev);
  CHECK_EQ(e.term(), 1);
  e.on_lead_request(request(3, 1, 2), 100 * kMs, ev);  // a second click, new counter
  CHECK_EQ(e.term(), 1);
  CHECK_EQ(count(ev, EventType::RoleChanged), 1u);
}

TEST(retransmitted_request_is_ignored_but_a_later_identical_counter_is_new) {
  LeaderElection e(cfg(1));
  std::vector<GatewayEvent> ev;
  e.start(0);
  e.on_lead_request(request(3, 1, 7), 0, ev);
  CHECK(e.is_master());
  // The peer takes over with a higher term; we step down.
  e.on_host_hb(peer_hb(2, Role::Master, 2, 2), 50 * kMs, ev);
  CHECK(e.role() == Role::Standby);
  // Same (sender, counter) 100 ms later: a retransmission, ignored.
  e.on_lead_request(request(3, 1, 7), 100 * kMs, ev);
  CHECK(e.role() == Role::Standby);
  // Same counter after 600 ms (sender restarted its counter): a fresh request.
  e.on_lead_request(request(3, 1, 7), 700 * kMs, ev);
  CHECK(e.is_master());
  CHECK_EQ(e.term(), 3);
}

TEST(higher_term_wins_and_the_old_master_steps_down) {
  LeaderElection orin(cfg(1));
  std::vector<GatewayEvent> ev;
  orin.start(0);
  orin.on_lead_request(request(3, 1, 1), 0, ev);
  CHECK_EQ(orin.term(), 1);
  ev.clear();
  orin.on_host_hb(peer_hb(2, Role::Master, 2, 2), 30 * kMs, ev);  // RPi claimed with term + 1
  CHECK(orin.role() == Role::Standby);
  CHECK_EQ(orin.believed_master(), 2);
  CHECK_EQ(orin.term(), 2);
  CHECK_EQ(count(ev, EventType::RoleChanged), 1u);
  CHECK(orin.take_hb_urgent());
}

TEST(a_master_ignores_a_lower_term_master) {
  LeaderElection e(cfg(1));
  std::vector<GatewayEvent> ev;
  e.start(0);
  e.on_host_hb(peer_hb(2, Role::Standby, 4), 0, ev);
  e.on_lead_request(request(3, 1, 1), 10 * kMs, ev);
  CHECK_EQ(e.term(), 5);
  e.on_host_hb(peer_hb(2, Role::Master, 3, 2), 20 * kMs, ev);  // stale claim
  CHECK(e.is_master());
  CHECK_EQ(e.term(), 5);
}

TEST(same_term_clash_goes_to_the_lower_tie_break) {
  LeaderElection orin(cfg(1)), rpi(cfg(2));
  std::vector<GatewayEvent> ev;
  orin.start(0);
  rpi.start(0);
  orin.on_lead_request(request(3, 1, 1), 0, ev);
  rpi.on_lead_request(request(3, 2, 1), 0, ev);  // both claim term 1 in the same instant
  CHECK(orin.is_master() && rpi.is_master());
  orin.on_host_hb(peer_hb(2, Role::Master, 1, 2), kMs, ev);
  rpi.on_host_hb(peer_hb(1, Role::Master, 1, 1), kMs, ev);
  CHECK(orin.is_master());   // tie-break 1 keeps it
  CHECK(!rpi.is_master());   // tie-break 2 yields
  CHECK_EQ(rpi.believed_master(), 1);
}

TEST(leader_lost_after_250_ms_without_a_master_heartbeat) {
  LeaderElection e(cfg(2));
  std::vector<GatewayEvent> ev;
  e.start(0);
  e.on_host_hb(peer_hb(1, Role::Master, 1, 1), 1000 * kMs, ev);  // last master heartbeat
  CHECK_EQ(e.believed_master(), 1);
  e.tick(1249 * kMs, ev);
  CHECK_EQ(count(ev, EventType::LeaderLost), 0u);
  e.tick(1251 * kMs, ev);
  CHECK_EQ(count(ev, EventType::LeaderLost), 1u);
  CHECK(e.leader_lost());
  CHECK_EQ(e.believed_master(), 0);
  e.tick(2000 * kMs, ev);
  CHECK_EQ(count(ev, EventType::LeaderLost), 1u);  // raised once, not every tick
  CHECK(e.role() == Role::Standby);                // and nothing restarts the rover by default
  CHECK_EQ(ev.back().node_id, 1);
}

TEST(a_master_heartbeat_keeps_the_leader_alive) {
  LeaderElection e(cfg(2));
  std::vector<GatewayEvent> ev;
  e.start(0);
  for (int64_t t = 0; t < 3000 * kMs; t += 50 * kMs) {
    e.on_host_hb(peer_hb(1, Role::Master, 1, 1), t, ev);
    e.tick(t + 10 * kMs, ev);
  }
  CHECK_EQ(count(ev, EventType::LeaderLost), 0u);
}

TEST(auto_takeover_claims_when_the_master_vanishes) {
  LeaderElection e(cfg(2, /*auto_takeover=*/true));
  std::vector<GatewayEvent> ev;
  e.start(0);
  e.on_host_hb(peer_hb(1, Role::Master, 4, 1), 0, ev);
  e.tick(200 * kMs, ev);
  CHECK(e.role() == Role::Standby);
  e.tick(260 * kMs, ev);
  CHECK(e.is_master());
  CHECK_EQ(e.term(), 5);
  CHECK_EQ(count(ev, EventType::LeaderLost), 1u);
  CHECK(e.take_hb_urgent());
}

TEST(auto_takeover_does_nothing_if_there_never_was_a_master) {
  LeaderElection e(cfg(2, true));
  std::vector<GatewayEvent> ev;
  e.start(0);
  e.on_host_hb(peer_hb(1, Role::Standby, 0), 0, ev);
  e.tick(10000 * kMs, ev);
  CHECK(e.role() == Role::Standby);
  CHECK(ev.empty());
}

TEST(an_orderly_step_down_is_not_a_lost_leader) {
  LeaderElection e(cfg(2));
  std::vector<GatewayEvent> ev;
  e.start(0);
  e.on_host_hb(peer_hb(1, Role::Master, 1, 1), 0, ev);
  e.on_host_hb(peer_hb(1, Role::Standby, 1, 0), 50 * kMs, ev);  // the master reports it stepped down
  e.tick(1000 * kMs, ev);
  CHECK_EQ(count(ev, EventType::LeaderLost), 0u);
  CHECK_EQ(e.believed_master(), 0);
}

TEST(terms_compare_across_the_u16_wrap) {
  CHECK(term_gt(0, 65535));
  CHECK(term_gt(5, 65530));
  CHECK(!term_gt(65535, 0));
  CHECK_EQ(term_max(65535, 1), 1);
  LeaderElection e(cfg(1));
  std::vector<GatewayEvent> ev;
  e.start(0);
  // Walk the peer's term up to the top of the u16 range in steps (each ahead of the last).
  for (uint16_t term : {20000, 40000, 60000, 65535}) e.on_host_hb(peer_hb(2, Role::Standby, term), kMs, ev);
  CHECK_EQ(e.term(), 65535);
  e.on_lead_request(request(3, 1, 1), 2 * kMs, ev);
  CHECK_EQ(e.term(), 0);  // 65535 + 1 wraps to 0, and still counts as newer than 65535
  CHECK(e.is_master());
}

int main() { return t::run_all(); }
