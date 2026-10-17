#include "anv/host/election.hpp"

namespace anv::host {

const char* to_string(Role r) {
  switch (r) {
    case Role::Booting: return "booting";
    case Role::Standby: return "standby";
    case Role::Leader: return "leader";
    case Role::Observer: return "observer";
  }
  return "?";
}

Election::Election(ElectionConfig cfg) : cfg_(cfg) {}

void Election::start() {
  if (role_ != Role::Booting) return;
  std::vector<ElectionEvent> ignored;
  set_role(cfg_.observer ? Role::Observer : Role::Standby, ignored);
}

bool Election::take_urgent() {
  const bool u = urgent_;
  urgent_ = false;
  return u;
}

void Election::set_role(Role r, std::vector<ElectionEvent>& events) {
  if (r == role_) return;
  role_ = r;
  urgent_ = true;
  ElectionEvent e{ElectionEvent::Kind::RoleChanged};
  e.host = cfg_.host_id;
  e.term = term_;
  e.role = r;
  events.push_back(e);
}

void Election::set_leader(uint8_t id, uint16_t term, int64_t now_ns, std::vector<ElectionEvent>& events) {
  leader_seen_ns_ = now_ns;
  if (id == leader_) return;
  leader_ = id;
  ElectionEvent e{ElectionEvent::Kind::LeaderChanged};
  e.host = id;
  e.term = term;
  events.push_back(e);
}

bool Election::beats_me(const HostHeartbeat& hb) const {
  if (hb.term != term_) return term_newer(hb.term, term_);
  if (hb.tie_break != cfg_.tie_break) return hb.tie_break < cfg_.tie_break;
  return hb.src < cfg_.host_id;
}

void Election::claim(int64_t now_ns, std::vector<ElectionEvent>& events) {
  // A repeated request must not bump the term again (that would disarm every node for nothing).
  if (role_ == Role::Leader || role_ == Role::Booting || cfg_.observer) return;
  highest_ = static_cast<uint16_t>(highest_ + 1);
  term_seen_ = true;
  term_ = highest_;
  set_role(Role::Leader, events);
  set_leader(cfg_.host_id, term_, now_ns, events);
}

void Election::resign(int64_t now_ns, std::vector<ElectionEvent>& events) {
  if (role_ != Role::Leader) return;
  set_role(Role::Standby, events);
  set_leader(0, term_, now_ns, events);
}

void Election::on_host_heartbeat(const HostHeartbeat& hb, int64_t now_ns, std::vector<ElectionEvent>& events) {
  if (hb.src == cfg_.host_id || hb.src == 0) return;
  PeerHost& p = peers_[hb.src];
  p.id = hb.src;
  p.role = hb.role;
  p.tie_break = hb.tie_break;
  p.term = hb.term;
  p.leader = hb.leader;
  p.last_seen_ns = now_ns;
  p.online = true;
  if (!term_seen_ || term_newer(hb.term, highest_)) highest_ = hb.term;
  term_seen_ = true;

  if (hb.role == Role::Leader) {
    if (role_ == Role::Leader) {
      // Rule 4. If we win, the other side steps down when it hears us.
      if (beats_me(hb)) {
        term_ = hb.term;
        set_role(Role::Standby, events);
        set_leader(hb.src, hb.term, now_ns, events);
      }
    } else if (leader_ == 0 || leader_ == hb.src || !term_newer(term_, hb.term)) {
      set_leader(hb.src, hb.term, now_ns, events);
    }
  } else if (role_ != Role::Leader && leader_ == hb.src) {
    // The leader says it no longer leads: an orderly step-down, not a loss.
    set_leader(0, hb.term, now_ns, events);
  }
  if (role_ != Role::Leader) term_ = highest_;
}

void Election::on_lead_request(uint8_t src, uint8_t leader, uint8_t counter, int64_t now_ns,
                               std::vector<ElectionEvent>& events) {
  // The counter makes a second button press new; the same one again soon is a resend.
  auto it = requests_.find(src);
  if (it != requests_.end() && it->second.counter == counter && now_ns - it->second.at_ns < cfg_.lead_request_dedup_ns) {
    return;
  }
  requests_[src] = {counter, now_ns};
  // A request naming another host needs nothing here: it claims a higher term and we step down.
  if (leader == cfg_.host_id) claim(now_ns, events);
}

void Election::tick(int64_t now_ns, std::vector<ElectionEvent>& events) {
  for (auto& kv : peers_) {
    if (kv.second.online && now_ns - kv.second.last_seen_ns > cfg_.peer_timeout_ns) kv.second.online = false;
  }
  if (role_ != Role::Leader && leader_ != 0 && now_ns - leader_seen_ns_ > cfg_.leader_timeout_ns) {
    ElectionEvent e{ElectionEvent::Kind::LeaderLost};
    e.host = leader_;
    e.term = term_;
    events.push_back(e);
    set_leader(0, term_, now_ns, events);
    // Nodes stay disarmed until someone sends ARM, so even this never restarts the rover.
    if (cfg_.auto_takeover) claim(now_ns, events);
  }

  // Two hosts still leading after a handover's normal overlap means a fault (e.g. one host
  // can't hear the other). Nodes follow the higher term, so this is a warning, not a stop.
  uint8_t a = role_ == Role::Leader ? cfg_.host_id : 0, b = 0;
  for (const auto& kv : peers_) {
    const PeerHost& p = kv.second;
    if (p.role != Role::Leader || now_ns - p.last_seen_ns > cfg_.leader_timeout_ns) continue;
    (a == 0 ? a : b) = p.id;
    if (b != 0) break;
  }
  if (b == 0) {
    two_leaders_since_ns_ = 0;
    two_leaders_reported_ = false;
  } else if (two_leaders_since_ns_ == 0) {
    two_leaders_since_ns_ = now_ns;
  } else if (!two_leaders_reported_ && now_ns - two_leaders_since_ns_ > cfg_.two_leaders_grace_ns) {
    two_leaders_reported_ = true;
    ElectionEvent e{ElectionEvent::Kind::TwoLeaders};
    e.host = a;
    e.other = b;
    e.term = term_;
    events.push_back(e);
  }
}

}  // namespace anv::host
