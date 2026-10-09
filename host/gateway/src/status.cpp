#include "anv_gateway/status.hpp"

#include <cstdio>

#include "ids.h"

namespace anv::gw {

const char* role_name(Role r) {
  switch (r) {
    case Role::Booting: return "BOOTING";
    case Role::Standby: return "STANDBY";
    case Role::Master: return "MASTER";
    case Role::Observer: return "OBSERVER";
  }
  return "?";
}

const char* node_state_name(uint8_t state) {
  switch (state) {
    case 0: return "BOOT";
    case 1: return "DISARMED";
    case 2: return "ARMED";
    case 3: return "FAULT";
    case 4: return "ESTOP";
    case 5: return "BOOTLOADER";
    default: return "?";
  }
}

namespace {

// Measured rate of one frame ID from the registry's rate table, -1 if never seen.
double rate_of(const GatewayState& s, uint8_t node_id, uint8_t cls, uint8_t sub) {
  for (const RateInfo& r : s.rates)
    if (r.node_id == node_id && ROVER_ID_CLASS(r.id) == cls && ROVER_ID_SUB(r.id) == sub) return r.measured_hz;
  return -1;
}

}  // namespace

std::string format_status(const GatewayState& s) {
  std::string out;
  char buf[256];

  std::snprintf(buf, sizeof buf, "host %u  %s  term %u  master %u%s%s%s\n", s.host_id, role_name(s.role), s.term,
                s.believed_master, s.leader_lost ? "  LEADER_LOST" : "", s.armed ? "  ARMED" : "",
                s.estop_latched ? "  ESTOP" : "");
  out += buf;

  std::snprintf(buf, sizeof buf, "bus   rx %llu (%.0f/s)  tx %llu  tx_fail %llu  err %llu  undecoded %llu%s\n",
                static_cast<unsigned long long>(s.bus.rx_frames), s.bus.rx_fps,
                static_cast<unsigned long long>(s.bus.tx_frames), static_cast<unsigned long long>(s.bus.tx_failures),
                static_cast<unsigned long long>(s.bus.error_frames),
                static_cast<unsigned long long>(s.bus.undecoded_frames), s.bus.bus_off ? "  BUS-OFF" : "");
  out += buf;

  std::snprintf(buf, sizeof buf, "cycle sent %llu  late %llu  worst lateness %lld us\n",
                static_cast<unsigned long long>(s.cycles_sent), static_cast<unsigned long long>(s.late_cycles),
                static_cast<long long>(s.worst_lateness_ns / 1000));
  out += buf;

  for (uint8_t h = 1; h <= 3; ++h) {
    const PeerHost& p = s.peers[h];
    if (!p.seen || h == s.host_id) continue;
    const bool alive = s.time_ns - p.last_seen_ns <= kLeaderLostNs;
    std::snprintf(buf, sizeof buf, "peer  host %u  %s  term %u  %s\n", h, role_name(static_cast<Role>(p.role)),
                  p.term, alive ? "alive" : "silent");
    out += buf;
  }

  for (const NodeInfo& n : s.nodes) {
    if (!n.seen) {
      std::snprintf(buf, sizeof buf, "node  %-7s never seen\n", n.name.c_str());
      out += buf;
      continue;
    }
    const double fb = rate_of(s, n.node_id, ROVER_CLASS_FEEDBACK, 0);
    const double hb = rate_of(s, n.node_id, ROVER_CLASS_HEARTBEAT, 0);
    std::snprintf(buf, sizeof buf,
                  "node  %-7s %-7s %-10s fw %04x  up %us  reboots %u  tec/rec %u/%u  hb %.0fHz fb %.0fHz\n",
                  n.name.c_str(), n.alive ? "alive" : "MISSING", node_state_name(n.state), n.fw_build_id, n.uptime_s,
                  n.reboot_count, n.tec, n.rec, hb < 0 ? 0 : hb, fb < 0 ? 0 : fb);
    out += buf;
  }
  return out;
}

}  // namespace anv::gw
