// Leader election, exactly as in docs/architecture.md "Hosts: choosing which one leads".
// Pure logic: it never touches the bus or the clock. Callers pass in decoded frames and the
// current time, and read back role, term and events. That makes every rule testable with a
// fake clock.
#pragma once

#include <array>
#include <cstdint>
#include <vector>

#include "anv_gateway/events.hpp"
#include "anv_gateway/messages.hpp"
#include "anv_gateway/protocol.hpp"

namespace anv::gw {

struct ElectionConfig {
  uint8_t host_id = kNodeOrin;    // 1 Orin, 2 RPi
  uint8_t tie_break = kNodeOrin;  // lower wins a same-term clash (Orin 1, RPi 2)
  bool auto_takeover = false;     // claim 250 ms after the master vanishes; off unless asked for
};

struct PeerHost {
  bool seen = false;
  uint8_t role = 0;
  uint8_t tie_break = 0;
  uint16_t term = 0;
  int64_t last_seen_ns = 0;
};

class LeaderElection {
 public:
  explicit LeaderElection(ElectionConfig cfg);

  // Rule 1: a host that boots never takes the lead by itself. Calling start() moves
  // Booting -> Standby; nothing else ever does.
  void start(int64_t now_ns);

  void on_host_hb(const HostHbMsg& hb, int64_t now_ns, std::vector<GatewayEvent>& events);
  // Also used for this host's own requests (own frames are not echoed back by the bus).
  void on_lead_request(const LeadRequestMsg& req, int64_t now_ns, std::vector<GatewayEvent>& events);
  // Call at least every ~10 ms. Raises LEADER_LOST and performs auto-takeover.
  void tick(int64_t now_ns, std::vector<GatewayEvent>& events);

  Role role() const { return role_; }
  bool is_master() const { return role_ == Role::Master; }
  // While master: this host's term. Otherwise: the highest term seen (what a claimant must beat).
  uint16_t term() const { return term_; }
  // Node ID this host believes is master, 0 if none.
  uint8_t believed_master() const { return master_id_; }
  bool leader_lost() const { return leader_lost_; }
  const ElectionConfig& config() const { return cfg_; }
  const PeerHost& peer(uint8_t host_node) const { return peers_[host_node < peers_.size() ? host_node : 0]; }

  // True once after a claim or step-down: the caller should send HOST_HB right away (claims must
  // be announced within 50 ms) instead of waiting for the next 20 Hz slot.
  bool take_hb_urgent();

 private:
  void claim(int64_t now_ns, std::vector<GatewayEvent>& events);
  void step_down(const HostHbMsg& winner, int64_t now_ns, std::vector<GatewayEvent>& events);
  void emit_role(int64_t now_ns, std::vector<GatewayEvent>& events) const;

  struct LastRequest {
    bool valid = false;
    uint8_t counter = 0;
    int64_t at_ns = 0;
  };

  ElectionConfig cfg_;
  Role role_ = Role::Booting;
  uint16_t term_ = 0;
  uint16_t highest_term_ = 0;
  uint8_t master_id_ = 0;
  uint16_t master_term_ = 0;
  int64_t last_master_hb_ns_ = 0;
  bool leader_lost_ = false;
  bool hb_urgent_ = false;
  std::array<PeerHost, 4> peers_{};          // indexed by host node 1-3
  std::array<LastRequest, 4> last_request_{};  // indexed by sender node 1-3
};

}  // namespace anv::gw
