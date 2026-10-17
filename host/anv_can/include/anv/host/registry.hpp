// Node registry: every node is discovered from its heartbeat, matched to a type in the schema
// by type_id, checked against the schema's wire hash, and tracked online/offline. Nothing is
// pre-seeded: the registry knows only what the bus has told it.
//
// Pure logic (no bus, no clock, no locking), like Election.
#pragma once

#include <cstdint>
#include <map>
#include <string>
#include <vector>

#include "anv/schema.hpp"

namespace anv::host {

// What a node's heartbeat says (wire integers; Host decodes the frame with the schema).
struct NodeHeartbeat {
  uint8_t src = 0;
  uint8_t state = 0;  // protocol node_state
  uint8_t can_state = 0;
  uint8_t debug_level = 0;
  bool unsaved_params = false;
  bool leader_seen = false;
  bool fault_active = false;
  uint16_t type_id = 0;
  uint16_t schema_hash = 0;
  uint16_t uptime_s = 0;
};

enum class Compat {
  Ok,              // the type is in our schema and the hashes match: safe to decode
  UnknownType,     // no type in our schema has this type_id
  SchemaMismatch,  // the type is known but the node was built from a different version of it
};
const char* to_string(Compat c);

struct NodeInfo {
  uint8_t id = 0;
  bool online = false;
  uint16_t type_id = 0;
  uint16_t schema_hash = 0;
  const NodeType* type = nullptr;  // our schema's type for type_id (even on a mismatch); nullptr if unknown
  Compat compat = Compat::UnknownType;
  NodeHeartbeat last;              // the latest heartbeat
  uint32_t reboots = 0;            // times the uptime went backwards
  int64_t first_seen_ns = 0, last_seen_ns = 0;
  double heartbeat_hz = 0;         // measured
  uint64_t mac = 0;                // from the ID allocator, 0 if not known

  // Decoding the node's own messages is only safe when the schemas agree.
  const NodeType* usable_type() const { return compat == Compat::Ok ? type : nullptr; }
};

struct RegistryConfig {
  // A node is offline when no heartbeat came for max(this, 3.5 heartbeat periods), so a node
  // whose heartbeat_hz was turned down doesn't flap.
  int64_t node_timeout_ns = 500'000'000;
};

struct RegistryEvent {
  enum class Kind {
    Appeared,      // first heartbeat, or back after Lost (`returned`), or a different type now
    Lost,          // heartbeats stopped
    Rebooted,      // uptime went backwards
    StateChanged,  // node_state changed (from `old_state`)
  } kind;
  uint8_t node = 0;
  bool returned = false;
  uint8_t old_state = 0;
};

class NodeRegistry {
 public:
  NodeRegistry(const Schema& schema, RegistryConfig cfg = {});

  void on_heartbeat(const NodeHeartbeat& hb, int64_t now_ns, std::vector<RegistryEvent>& events);
  void tick(int64_t now_ns, std::vector<RegistryEvent>& events);
  void set_mac(uint8_t node, uint64_t mac);
  void forget(uint8_t node);

  const NodeInfo* find(uint8_t node) const;
  const std::map<uint8_t, NodeInfo>& nodes() const { return nodes_; }
  bool online(uint8_t node) const;

 private:
  const Schema& schema_;
  RegistryConfig cfg_;
  std::map<uint8_t, NodeInfo> nodes_;
  std::map<uint8_t, uint64_t> macs_;
};

}  // namespace anv::host
