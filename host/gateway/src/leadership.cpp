#include "anv_gateway/leadership.hpp"

namespace anv::gw {

LeaderElection::LeaderElection(ElectionConfig cfg) : cfg_(cfg) {}

void LeaderElection::start(int64_t /*now_ns*/) {
  if (role_ == Role::Booting) role_ = Role::Standby;
}

bool LeaderElection::take_hb_urgent() {
  const bool u = hb_urgent_;
  hb_urgent_ = false;
  return u;
}

void LeaderElection::emit_role(int64_t now_ns, std::vector<GatewayEvent>& events) const {
  GatewayEvent e;
  e.type = EventType::RoleChanged;
  e.time_ns = now_ns;
  e.node_id = cfg_.host_id;
  e.term = term_;
  e.role = static_cast<uint8_t>(role_);
  events.push_back(e);
}

void LeaderElection::claim(int64_t now_ns, std::vector<GatewayEvent>& events) {
  if (role_ == Role::Master) return;  // already leading: a repeat request must not bump the term
  // Rule 3: term = highest term seen + 1. Nodes disarm on a term change, so this is also what
  // makes every handover stop the rover first.
  highest_term_ = term_max(highest_term_, term_);
  term_ = static_cast<uint16_t>(highest_term_ + 1);
  highest_term_ = term_;
  role_ = Role::Master;
  master_id_ = cfg_.host_id;
  master_term_ = term_;
  leader_lost_ = false;
  hb_urgent_ = true;
  emit_role(now_ns, events);
}

void LeaderElection::step_down(const HostHbMsg& winner, int64_t now_ns, std::vector<GatewayEvent>& events) {
  // Rule 4: higher term wins; we stop commanding at once and just follow.
  role_ = Role::Standby;
  term_ = highest_term_;
  master_id_ = winner.host_node;
  master_term_ = winner.term;
  last_master_hb_ns_ = now_ns;
  leader_lost_ = false;
  hb_urgent_ = true;
  emit_role(now_ns, events);
}

void LeaderElection::on_host_hb(const HostHbMsg& hb, int64_t now_ns, std::vector<GatewayEvent>& events) {
  if (hb.host_node == cfg_.host_id || hb.host_node == 0 || hb.host_node >= peers_.size()) return;

  PeerHost& p = peers_[hb.host_node];
  p.seen = true;
  p.role = hb.role;
  p.tie_break = hb.tie_break;
  p.term = hb.term;
  p.last_seen_ns = now_ns;

  highest_term_ = term_max(highest_term_, hb.term);

  if (hb.role == static_cast<uint8_t>(Role::Master)) {
    if (role_ == Role::Master) {
      const bool higher = term_gt(hb.term, term_);
      const bool same_term_and_wins = hb.term == term_ && hb.tie_break < cfg_.tie_break;
      if (higher || same_term_and_wins) step_down(hb, now_ns, events);
      // Otherwise we hold the higher term (or win the tie): the other master steps down on seeing us.
    } else if (master_id_ == 0 || master_id_ == hb.host_node || term_ge(hb.term, master_term_)) {
      master_id_ = hb.host_node;
      master_term_ = hb.term;
      last_master_hb_ns_ = now_ns;
      leader_lost_ = false;
      term_ = term_max(term_, hb.term);
    }
  } else if (role_ != Role::Master && master_id_ == hb.host_node) {
    // The master itself reports it is no longer master: an orderly step-down, not a loss.
    master_id_ = 0;
  }
  if (role_ != Role::Master) term_ = highest_term_;
}

void LeaderElection::on_lead_request(const LeadRequestMsg& req, int64_t now_ns,
                                      std::vector<GatewayEvent>& events) {
  if (req.sender_node >= last_request_.size()) return;
  LastRequest& last = last_request_[req.sender_node];
  // The counter exists so a repeated button press counts as new. The same (sender, counter)
  // twice within half a second is a retransmission, not a second click.
  if (last.valid && last.counter == req.request_counter && now_ns - last.at_ns < kLeadRequestDedupNs) return;
  last = {true, req.request_counter, now_ns};

  // Rule 2: claim only when a request names this host. A request naming the other host needs no
  // action here: that host claims with a higher term and we step down when we hear it (rule 5).
  if (req.requested_leader == cfg_.host_id) claim(now_ns, events);
}

void LeaderElection::tick(int64_t now_ns, std::vector<GatewayEvent>& events) {
  if (role_ == Role::Standby && master_id_ != 0 && now_ns - last_master_hb_ns_ > kLeaderLostNs) {
    GatewayEvent e;
    e.type = EventType::LeaderLost;
    e.time_ns = now_ns;
    e.node_id = master_id_;
    e.term = master_term_;
    events.push_back(e);
    master_id_ = 0;
    leader_lost_ = true;
    // Optional auto-takeover (default off): claim now. Nodes stay disarmed until someone ARMs.
    if (cfg_.auto_takeover) claim(now_ns, events);
  }
}

}  // namespace anv::gw
