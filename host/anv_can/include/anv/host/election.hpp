// Leader election between hosts (docs/architecture.md "Hosts: choosing which one leads"),
// generalised: any number of hosts with any IDs, nothing assumes which machines they are.
//
// Pure logic: no bus, no clock. The caller feeds decoded host_heartbeat and lead_request
// frames with the time and reads back the role, term and events, so every rule is testable
// with a fake clock.
//
// The rules:
//  1. A host starts as standby and never takes the lead by itself.
//  2. It claims only when a lead_request names it (or, with auto_takeover, when the leader
//     has gone quiet for leader_timeout).
//  3. Claiming sets term = highest term seen + 1. Nodes disarm on a new term, so every
//     handover stops the rover first.
//  4. Higher term wins (serial-number arithmetic, so the 16-bit term may wrap). Same term:
//     lower tie_break wins, then lower host ID.
#pragma once

#include <cstdint>
#include <map>
#include <vector>

namespace anv::host {

// Numbers are the protocol's host_role enum; Host maps them from the schema by label.
enum class Role : uint8_t { Booting, Standby, Leader, Observer };
const char* to_string(Role r);

struct ElectionConfig {
  uint8_t host_id = 0;
  uint8_t tie_break = 0;
  bool observer = false;        // never leads (tools, loggers)
  bool auto_takeover = false;   // claim when the leader vanishes; off unless asked for
  int64_t leader_timeout_ns = 250'000'000;
  int64_t peer_timeout_ns = 1'000'000'000;    // a host not heard for this long is gone
  int64_t two_leaders_grace_ns = 200'000'000;  // a handover overlaps for one heartbeat; longer is a fault
  int64_t lead_request_dedup_ns = 500'000'000;  // same (sender, counter) again = a resend
};

struct HostHeartbeat {
  uint8_t src = 0;
  Role role = Role::Standby;
  uint8_t tie_break = 0;
  uint16_t term = 0;
  uint8_t leader = 0;
};

struct PeerHost {
  uint8_t id = 0;
  Role role = Role::Standby;
  uint8_t tie_break = 0;
  uint16_t term = 0;
  uint8_t leader = 0;
  int64_t last_seen_ns = 0;
  bool online = false;
};

struct ElectionEvent {
  enum class Kind {
    RoleChanged,    // this host's role changed (role, term)
    LeaderChanged,  // the leader is now `host` (0 = none) at `term`
    LeaderLost,     // the leader `host` stopped sending heartbeats
    TwoLeaders,     // `host` and `other` both kept claiming the lead for longer than the grace
  } kind;
  uint8_t host = 0;
  uint8_t other = 0;
  uint16_t term = 0;
  Role role = Role::Standby;
};

// a is newer than b, with wrap-around.
inline bool term_newer(uint16_t a, uint16_t b) { return static_cast<int16_t>(a - b) > 0; }

class Election {
 public:
  explicit Election(ElectionConfig cfg);

  void start();  // Booting -> Standby (or Observer). Nothing else leaves Booting.
  void on_host_heartbeat(const HostHeartbeat& hb, int64_t now_ns, std::vector<ElectionEvent>& events);
  // Feed every lead_request, including this host's own (the bus doesn't echo them back).
  void on_lead_request(uint8_t src, uint8_t leader, uint8_t counter, int64_t now_ns,
                       std::vector<ElectionEvent>& events);
  // Call every few ms: leader timeout, auto-takeover, peers going offline.
  void tick(int64_t now_ns, std::vector<ElectionEvent>& events);
  // Give up the lead (e.g. the operator stops this host leading); peers keep the term.
  void resign(int64_t now_ns, std::vector<ElectionEvent>& events);

  Role role() const { return role_; }
  bool leading() const { return role_ == Role::Leader; }
  // While leading: our term. Otherwise the highest term seen (what a claim must beat).
  uint16_t term() const { return term_; }
  uint8_t leader() const { return leader_; }  // 0 = none known
  const ElectionConfig& config() const { return cfg_; }
  const std::map<uint8_t, PeerHost>& peers() const { return peers_; }

  // True once after a claim or step-down: send host_heartbeat now instead of at the next slot.
  bool take_urgent();

 private:
  void claim(int64_t now_ns, std::vector<ElectionEvent>& events);
  void set_role(Role r, std::vector<ElectionEvent>& events);
  void set_leader(uint8_t id, uint16_t term, int64_t now_ns, std::vector<ElectionEvent>& events);
  bool beats_me(const HostHeartbeat& hb) const;

  struct LastRequest {
    uint8_t counter = 0;
    int64_t at_ns = 0;
  };

  ElectionConfig cfg_;
  Role role_ = Role::Booting;
  uint16_t term_ = 0;
  uint16_t highest_ = 0;  // highest term seen anywhere, ours included
  bool term_seen_ = false;
  uint8_t leader_ = 0;
  int64_t leader_seen_ns_ = 0;
  bool urgent_ = false;
  std::map<uint8_t, PeerHost> peers_;
  std::map<uint8_t, LastRequest> requests_;
  int64_t two_leaders_since_ns_ = 0;
  bool two_leaders_reported_ = false;
};

}  // namespace anv::host
