// Leader election rules with a fake clock. Host IDs here are arbitrary on purpose: nothing may
// assume which machines the hosts are.
#include "anv/host/election.hpp"
#include "check.hpp"

using namespace anv::host;

namespace {

constexpr int64_t kMs = 1'000'000;

ElectionConfig cfg(uint8_t id, uint8_t tie = 0) {
  ElectionConfig c;
  c.host_id = id;
  c.tie_break = tie ? tie : id;
  return c;
}

HostHeartbeat hb(uint8_t src, Role role, uint16_t term, uint8_t tie = 0, uint8_t leader = 0) {
  HostHeartbeat h;
  h.src = src;
  h.role = role;
  h.term = term;
  h.tie_break = tie ? tie : src;
  h.leader = leader;
  return h;
}

bool has(const std::vector<ElectionEvent>& ev, ElectionEvent::Kind k) {
  for (const auto& e : ev) {
    if (e.kind == k) return true;
  }
  return false;
}

}  // namespace

TEST(starts_standby_never_claims_alone) {
  Election e(cfg(7));
  CHECK(e.role() == Role::Booting);
  e.start();
  CHECK(e.role() == Role::Standby);
  std::vector<ElectionEvent> ev;
  for (int64_t t = 0; t < 5000 * kMs; t += 10 * kMs) e.tick(t, ev);
  CHECK(e.role() == Role::Standby && e.leader() == 0 && ev.empty());
}

TEST(observer_never_leads) {
  ElectionConfig c = cfg(9);
  c.observer = true;
  Election e(c);
  e.start();
  std::vector<ElectionEvent> ev;
  e.on_lead_request(1, 9, 1, 0, ev);
  CHECK(e.role() == Role::Observer && !e.leading());
}

TEST(claims_when_named_with_higher_term_once) {
  Election e(cfg(7));
  e.start();
  std::vector<ElectionEvent> ev;
  e.on_host_heartbeat(hb(3, Role::Standby, 41), 0, ev);
  e.on_lead_request(3, 7, 1, 10 * kMs, ev);  // someone else's button names us
  CHECK(e.leading() && e.term() == 42 && e.leader() == 7);
  CHECK(has(ev, ElectionEvent::Kind::RoleChanged) && has(ev, ElectionEvent::Kind::LeaderChanged));
  CHECK(e.take_urgent() && !e.take_urgent());
  // The same press resent, and a new press while already leading: no new term.
  e.on_lead_request(3, 7, 1, 20 * kMs, ev);
  e.on_lead_request(3, 7, 2, 30 * kMs, ev);
  CHECK(e.term() == 42);
}

TEST(request_for_other_host_does_nothing_here) {
  Election e(cfg(7));
  e.start();
  std::vector<ElectionEvent> ev;
  e.on_lead_request(7, 3, 1, 0, ev);
  CHECK(!e.leading() && ev.empty());
}

TEST(higher_term_wins_and_handover) {
  Election a(cfg(7)), b(cfg(3));
  a.start();
  b.start();
  std::vector<ElectionEvent> ev;
  a.on_lead_request(7, 7, 1, 0, ev);
  CHECK(a.leading() && a.term() == 1);
  b.on_host_heartbeat(hb(7, Role::Leader, 1), 1 * kMs, ev);
  CHECK(b.leader() == 7 && b.term() == 1);
  // Operator hands the lead to 3: it claims term 2; 7 hears it and steps down.
  b.on_lead_request(7, 3, 2, 2 * kMs, ev);
  CHECK(b.leading() && b.term() == 2);
  ev.clear();
  a.on_host_heartbeat(hb(3, Role::Leader, 2), 3 * kMs, ev);
  CHECK(!a.leading() && a.role() == Role::Standby && a.leader() == 3 && a.term() == 2);
  CHECK(has(ev, ElectionEvent::Kind::RoleChanged));
  // 3 keeps its lead when it hears 7's stale leader heartbeat with the old term.
  b.on_host_heartbeat(hb(7, Role::Leader, 1), 4 * kMs, ev);
  CHECK(b.leading());
}

TEST(same_term_tie_break_then_id) {
  Election a(cfg(7, 2));
  a.start();
  std::vector<ElectionEvent> ev;
  a.on_lead_request(7, 7, 1, 0, ev);
  a.on_host_heartbeat(hb(9, Role::Leader, 1, 5), 1 * kMs, ev);  // worse tie-break: we keep it
  CHECK(a.leading());
  a.on_host_heartbeat(hb(9, Role::Leader, 1, 1), 2 * kMs, ev);  // better tie-break: we step down
  CHECK(!a.leading() && a.leader() == 9);

  Election c(cfg(7, 4));
  c.start();
  c.on_lead_request(7, 7, 1, 0, ev);
  c.on_host_heartbeat(hb(9, Role::Leader, 1, 4), 1 * kMs, ev);  // same tie-break: lower ID wins
  CHECK(c.leading());
  c.on_host_heartbeat(hb(5, Role::Leader, 1, 4), 2 * kMs, ev);
  CHECK(!c.leading() && c.leader() == 5);
}

TEST(term_wraps) {
  CHECK(term_newer(1, 0xFFFF) && !term_newer(0xFFFF, 1) && !term_newer(5, 5));
  Election e(cfg(7));
  e.start();
  std::vector<ElectionEvent> ev;
  e.on_host_heartbeat(hb(3, Role::Standby, 0xFFFF), 0, ev);
  e.on_lead_request(3, 7, 1, 0, ev);
  CHECK(e.leading() && e.term() == 0);
}

TEST(leader_lost_and_auto_takeover) {
  Election e(cfg(7));
  e.start();
  std::vector<ElectionEvent> ev;
  e.on_host_heartbeat(hb(3, Role::Leader, 4), 0, ev);
  CHECK(e.leader() == 3);
  ev.clear();
  e.tick(200 * kMs, ev);
  CHECK(e.leader() == 3 && ev.empty());
  e.tick(300 * kMs, ev);
  CHECK(e.leader() == 0 && has(ev, ElectionEvent::Kind::LeaderLost) && !e.leading());

  ElectionConfig c = cfg(7);
  c.auto_takeover = true;
  Election a(c);
  a.start();
  a.on_host_heartbeat(hb(3, Role::Leader, 4), 0, ev);
  a.tick(300 * kMs, ev);
  CHECK(a.leading() && a.term() == 5);
}

TEST(orderly_step_down_is_not_a_loss) {
  Election e(cfg(7));
  e.start();
  std::vector<ElectionEvent> ev;
  e.on_host_heartbeat(hb(3, Role::Leader, 4), 0, ev);
  e.on_host_heartbeat(hb(3, Role::Standby, 4), 10 * kMs, ev);
  ev.clear();
  e.tick(1000 * kMs, ev);
  CHECK(e.leader() == 0 && !has(ev, ElectionEvent::Kind::LeaderLost));
}

TEST(resign) {
  Election e(cfg(7));
  e.start();
  std::vector<ElectionEvent> ev;
  e.on_lead_request(7, 7, 1, 0, ev);
  e.resign(1, ev);
  CHECK(!e.leading() && e.leader() == 0 && e.term() == 1);
}

TEST(two_leaders_after_grace) {
  Election e(cfg(7));
  e.start();
  std::vector<ElectionEvent> ev;
  // Two other hosts both keep claiming (e.g. they can't hear each other).
  for (int64_t t = 0; t <= 400 * kMs; t += 50 * kMs) {
    e.on_host_heartbeat(hb(3, Role::Leader, 4), t, ev);
    e.on_host_heartbeat(hb(5, Role::Leader, 5), t, ev);
    e.tick(t, ev);
  }
  int n = 0;
  for (const auto& x : ev) n += x.kind == ElectionEvent::Kind::TwoLeaders;
  CHECK(n == 1);
  // A normal handover overlaps for one heartbeat only: no warning.
  Election f(cfg(7));
  f.start();
  ev.clear();
  f.on_host_heartbeat(hb(3, Role::Leader, 4), 0, ev);
  f.on_host_heartbeat(hb(5, Role::Leader, 5), 10 * kMs, ev);
  f.tick(20 * kMs, ev);
  f.on_host_heartbeat(hb(3, Role::Standby, 5), 50 * kMs, ev);
  for (int64_t t = 60 * kMs; t < 600 * kMs; t += 50 * kMs) {
    f.on_host_heartbeat(hb(5, Role::Leader, 5), t, ev);
    f.tick(t, ev);
  }
  CHECK(!has(ev, ElectionEvent::Kind::TwoLeaders) && f.leader() == 5);
}

TEST(peers_go_offline) {
  Election e(cfg(7));
  e.start();
  std::vector<ElectionEvent> ev;
  e.on_host_heartbeat(hb(3, Role::Standby, 0), 0, ev);
  CHECK(e.peers().at(3).online);
  e.tick(2000 * kMs, ev);
  CHECK(!e.peers().at(3).online);
}

TEST_MAIN()
