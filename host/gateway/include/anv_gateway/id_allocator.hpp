// Automatic node IDs (docs/architecture.md "Automatic node IDs"): a board with no stored ID sends
// NODE_ID_REQUEST with its MAC; the allocator host answers with an ID from the auxiliary pool and
// remembers the pairing, so the same board always gets the same ID back.
//
// The table is a small text file, one board per line: "<mac as 12 hex digits> <node id> <hw target>".
// Single-threaded: the gateway guards it with its state mutex.
#pragma once

#include <cstdint>
#include <functional>
#include <map>
#include <string>
#include <vector>

namespace anv::gw {

struct IdAssignment {
  uint64_t mac = 0;
  uint8_t node_id = 0;
  uint8_t hw_target = 0xFF;
};

class IdAllocator {
 public:
  explicit IdAllocator(std::string table_path);  // empty path = keep the table in memory only

  // A missing file is an empty table, not an error.
  bool load(std::string* error);

  // The ID for this MAC: its existing one, or the lowest free pool ID (16-19, then 22-30) that
  // `in_use` doesn't claim. Returns 0 when the pool is exhausted. Saves the table on change; a
  // failed save still returns the ID (the board gets an address) and fills *save_error.
  uint8_t assign(uint64_t mac, uint8_t hw_target, const std::function<bool(uint8_t)>& in_use, std::string* save_error);

  bool forget_node(uint8_t node_id, std::string* save_error);  // false if no board holds that ID
  const IdAssignment* find_by_node(uint8_t node_id) const;
  std::vector<IdAssignment> table() const;

  static std::vector<uint8_t> pool();
  static std::string mac_to_string(uint64_t mac);  // "a4:cf:12:34:56:78"

 private:
  bool save(std::string* error) const;

  std::string path_;
  std::map<uint64_t, IdAssignment> by_mac_;
};

}  // namespace anv::gw
