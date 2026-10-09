#include "anv_gateway/node_registry.hpp"

#include <algorithm>
#include <type_traits>

#include "anv_gateway/protocol.hpp"
#include "errors.h"
#include "ids.h"

namespace anv::gw {

namespace {
constexpr int64_t kRateWindowNs = kNsPerSec;

bool is_node_originated(uint16_t id) {
  const uint8_t cls = ROVER_ID_CLASS(id);
  // feedback, telemetry, sensors, heartbeat: the node is the sender, and the node field says which
  return cls == ROVER_CLASS_FEEDBACK || cls == ROVER_CLASS_TELEMETRY || cls == ROVER_CLASS_SENSOR ||
         cls == ROVER_CLASS_HEARTBEAT;
}
}  // namespace

std::string node_name(uint8_t node_id) {
  if (node_id >= kNodeFirstDrive && node_id < kNodeFirstDrive + kNumDrives)
    return "DRIVE" + std::to_string(node_id - 3);
  if (node_id == kNodeFpga) return "FPGA";
  if (node_id == kNodeOrin) return "ORIN";
  if (node_id == kNodeRpi) return "RPI";
  if (node_id == kNodeTool) return "TOOL";
  return "NODE" + std::to_string(node_id);
}

NodeRegistry::NodeRegistry() {
  for (uint8_t d = 1; d <= kNumDrives; ++d) entry(drive_node_id(d));
  entry(kNodeFpga);
}

NodeInfo& NodeRegistry::entry(uint8_t node_id) {
  auto it = nodes_.find(node_id);
  if (it == nodes_.end()) {
    NodeInfo n;
    n.node_id = node_id;
    n.name = node_name(node_id);
    it = nodes_.emplace(node_id, std::move(n)).first;
  }
  return it->second;
}

double NodeRegistry::expected_hz(uint16_t id, uint8_t debug_level) {
  const uint8_t cls = ROVER_ID_CLASS(id);
  const uint8_t sub = ROVER_ID_SUB(id);
  switch (cls) {
    case ROVER_CLASS_FEEDBACK: return 50.0;
    case ROVER_CLASS_SENSOR: return 50.0;
    case ROVER_CLASS_HEARTBEAT: return 10.0;
    case ROVER_CLASS_TELEMETRY:
      if (sub == 0 || sub == 1) return debug_level == 0 ? 1.0 : 10.0;  // 1 Hz at debug level 0
      if (sub == 2 || sub == 3) return debug_level == 2 ? 10.0 : (debug_level >= 3 ? 50.0 : 0.0);
      return 0.0;  // NODE_EVENT: only when something happens
    default: return 0.0;
  }
}

void NodeRegistry::touch(uint8_t node_id, int64_t rx_ns, std::vector<GatewayEvent>& events) {
  NodeInfo& n = entry(node_id);
  const bool was_seen = n.seen;
  n.last_seen_ns = rx_ns;
  n.seen = true;
  if (!n.alive) {
    n.alive = true;
    GatewayEvent e;
    e.type = EventType::NodeAlive;
    e.time_ns = rx_ns;
    e.node_id = node_id;
    // First-ever contact has no error code; coming back after being missing does (NODE_RETURNED).
    if (was_seen) {
      e.code = ROVER_ERR_NODE_RETURNED;
      e.severity = static_cast<uint8_t>(ROVER_SEVERITY_INFO);
    }
    events.push_back(e);
  }
}

void NodeRegistry::on_decoded(const Decoded& d, std::vector<GatewayEvent>& events) {
  if (!is_node_originated(d.id)) return;
  const uint8_t node_id = ROVER_ID_NODE(d.id);

  // Rate window bookkeeping, per frame ID.
  RateStat& r = rates_[d.id];
  if (!r.started) {
    r.started = true;
    r.window_start_ns = d.rx_ns;
  }
  ++r.count;

  touch(node_id, d.rx_ns, events);
  NodeInfo& n = entry(node_id);

  std::visit(
      [&](const auto& m) {
        using T = std::decay_t<decltype(m)>;
        if constexpr (std::is_same_v<T, NodeHbMsg>) {
          // A reboot shows up as the uptime counter going backwards. The u16 wraps after ~18 h;
          // a drop from the top of the range to near zero is that wrap, not a reboot.
          const bool wrapped = n.uptime_s >= 65500 && m.uptime_s <= 36;
          if (n.has_hb && m.uptime_s < n.uptime_s && !wrapped) {
            ++n.reboot_count;
            GatewayEvent e;
            e.type = EventType::NodeRebooted;
            e.time_ns = d.rx_ns;
            e.node_id = node_id;
            e.code = ROVER_ERR_NODE_REBOOTED;
            e.severity = static_cast<uint8_t>(ROVER_SEVERITY_WARNING);
            events.push_back(e);
          }
          n.has_hb = true;
          n.state = m.state;
          n.node_type = m.node_type;
          n.protocol_version = m.protocol_version;
          n.can_error_state = m.can_error_state;
          n.bootloader_running = m.bootloader_running != 0;
          n.unsaved_param_changes = m.unsaved_param_changes != 0;
          n.debug_level = m.debug_level;
          n.fw_build_id = m.fw_build_id;
          n.uptime_s = m.uptime_s;
        } else if constexpr (std::is_same_v<T, DriveFbAMsg>) {
          n.fb_a = m;
          n.has_fb_a = true;
          n.state = m.state;
        } else if constexpr (std::is_same_v<T, DriveFbBMsg>) {
          n.fb_b = m;
          n.has_fb_b = true;
        } else if constexpr (std::is_same_v<T, DriveTelem0Msg>) {
          n.telem0 = m;
          n.has_telem0 = true;
          n.tec = m.tec;
          n.rec = m.rec;
        } else if constexpr (std::is_same_v<T, DriveTelem1Msg>) {
          n.telem1 = m;
          n.has_telem1 = true;
        } else if constexpr (std::is_same_v<T, FpgaSensorsMsg>) {
          const size_t base = m.second_half ? 4 : 0;
          for (size_t i = 0; i < 4; ++i) n.sensors[base + i] = m.values[i];
          n.has_sensors = true;
        }
      },
      d.msg);
}

void NodeRegistry::tick(int64_t now_ns, std::vector<GatewayEvent>& events) {
  for (auto& [id, r] : rates_) {
    if (r.started && now_ns - r.window_start_ns >= kRateWindowNs) {
      r.hz = static_cast<double>(r.count) * kNsPerSec / static_cast<double>(now_ns - r.window_start_ns);
      r.count = 0;
      r.window_start_ns = now_ns;
    }
  }
  for (auto& [id, n] : nodes_) {
    if (n.alive && now_ns - n.last_seen_ns > kNodeMissingNs) {
      n.alive = false;
      GatewayEvent e;
      e.type = EventType::NodeMissing;
      e.time_ns = now_ns;
      e.node_id = id;
      e.code = ROVER_ERR_NODE_MISSING;
      e.severity = static_cast<uint8_t>(ROVER_SEVERITY_ERROR);
      events.push_back(e);
    }
  }
}

std::vector<NodeInfo> NodeRegistry::snapshot(int64_t now_ns) const {
  std::vector<NodeInfo> out;
  out.reserve(nodes_.size());
  for (const auto& [id, n] : nodes_) {
    NodeInfo copy = n;
    copy.alive = n.alive && now_ns - n.last_seen_ns <= kNodeMissingNs;
    out.push_back(std::move(copy));
  }
  return out;
}

std::vector<RateInfo> NodeRegistry::rates() const {
  std::vector<RateInfo> out;
  out.reserve(rates_.size());
  for (const auto& [id, r] : rates_) {
    RateInfo info;
    info.id = id;
    info.node_id = ROVER_ID_NODE(id);
    info.measured_hz = r.hz;
    const auto it = nodes_.find(info.node_id);
    info.expected_hz = expected_hz(id, it == nodes_.end() ? 0 : it->second.debug_level);
    info.low = info.expected_hz > 0 && info.measured_hz < 0.8 * info.expected_hz;
    out.push_back(info);
  }
  std::sort(out.begin(), out.end(), [](const RateInfo& a, const RateInfo& b) { return a.id < b.id; });
  return out;
}

}  // namespace anv::gw
