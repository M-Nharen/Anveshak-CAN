#include "anv/host/registry.hpp"

#include <algorithm>

namespace anv::host {

const char* to_string(Compat c) {
  switch (c) {
    case Compat::Ok: return "ok";
    case Compat::UnknownType: return "unknown_type";
    case Compat::SchemaMismatch: return "schema_mismatch";
  }
  return "?";
}

NodeRegistry::NodeRegistry(const Schema& schema, RegistryConfig cfg) : schema_(schema), cfg_(cfg) {}

void NodeRegistry::on_heartbeat(const NodeHeartbeat& hb, int64_t now_ns, std::vector<RegistryEvent>& events) {
  auto it = nodes_.find(hb.src);
  const bool is_new = it == nodes_.end();
  NodeInfo& n = nodes_[hb.src];
  const bool type_changed = !is_new && (n.type_id != hb.type_id || n.schema_hash != hb.schema_hash);
  const bool returned = !is_new && !n.online;

  if (is_new) {
    n.id = hb.src;
    n.first_seen_ns = now_ns;
    auto mac = macs_.find(hb.src);
    if (mac != macs_.end()) n.mac = mac->second;
  } else if (n.online) {
    const double dt = static_cast<double>(now_ns - n.last_seen_ns) / 1e9;
    if (dt > 0) {
      const double hz = 1.0 / dt;
      n.heartbeat_hz = n.heartbeat_hz == 0 ? hz : 0.8 * n.heartbeat_hz + 0.2 * hz;
    }
  }
  if (is_new || type_changed) {
    n.type_id = hb.type_id;
    n.schema_hash = hb.schema_hash;
    n.type = schema_.type_by_id(hb.type_id);
    n.compat = n.type == nullptr                         ? Compat::UnknownType
               : n.type->schema_hash16() != hb.schema_hash ? Compat::SchemaMismatch
                                                           : Compat::Ok;
  }
  if (!is_new && !type_changed && hb.uptime_s < n.last.uptime_s) {
    ++n.reboots;
    events.push_back({RegistryEvent::Kind::Rebooted, hb.src});
  }
  const uint8_t old_state = n.last.state;
  n.last = hb;
  n.last_seen_ns = now_ns;
  n.online = true;

  if (is_new || type_changed || returned) {
    RegistryEvent e{RegistryEvent::Kind::Appeared, hb.src};
    e.returned = returned && !type_changed;
    events.push_back(e);
  } else if (old_state != hb.state) {
    RegistryEvent e{RegistryEvent::Kind::StateChanged, hb.src};
    e.old_state = old_state;
    events.push_back(e);
  }
}

void NodeRegistry::tick(int64_t now_ns, std::vector<RegistryEvent>& events) {
  for (auto& kv : nodes_) {
    NodeInfo& n = kv.second;
    if (!n.online) continue;
    int64_t timeout = cfg_.node_timeout_ns;
    if (n.heartbeat_hz > 0) timeout = std::max(timeout, static_cast<int64_t>(3.5e9 / n.heartbeat_hz));
    if (now_ns - n.last_seen_ns > timeout) {
      n.online = false;
      events.push_back({RegistryEvent::Kind::Lost, n.id});
    }
  }
}

void NodeRegistry::set_mac(uint8_t node, uint64_t mac) {
  macs_[node] = mac;
  auto it = nodes_.find(node);
  if (it != nodes_.end()) it->second.mac = mac;
}

void NodeRegistry::forget(uint8_t node) {
  nodes_.erase(node);
  macs_.erase(node);
}

const NodeInfo* NodeRegistry::find(uint8_t node) const {
  auto it = nodes_.find(node);
  return it == nodes_.end() ? nullptr : &it->second;
}

bool NodeRegistry::online(uint8_t node) const {
  const NodeInfo* n = find(node);
  return n != nullptr && n->online;
}

}  // namespace anv::host
