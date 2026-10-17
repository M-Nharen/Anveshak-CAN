// The ID assignment server (the host side of automatic node IDs). A board with no ID sends
// id_request with its MAC and type; the allocator answers with an ID from its range and
// remembers the pairing, so the same board always gets the same ID back.
//
// The table is a small text file, one board per line: "<MAC, 12 hex digits> <node ID> <type_id>".
// Not thread-safe: Host guards it.
#pragma once

#include <cstdint>
#include <functional>
#include <map>
#include <string>
#include <vector>

namespace anv::host {

struct Assignment {
  uint64_t mac = 0;
  uint8_t node_id = 0;
  uint16_t type_id = 0;
};

class IdAllocator {
 public:
  // IDs first..last (from config; the schema's node_ids bound them). Empty path: memory only.
  IdAllocator(uint8_t first, uint8_t last, std::string table_path);

  // A missing file is an empty table, not an error.
  bool load(std::string* error);

  // The board's ID: its remembered one unless another board is using it, else the lowest ID
  // in range that no board holds and `in_use` doesn't claim, else (range full) the lowest one
  // not in use, taken from a board that isn't around. 0 when every ID is in use. Saves on
  // change; a failed save still returns the ID and fills *save_error.
  uint8_t assign(uint64_t mac, uint16_t type_id, const std::function<bool(uint8_t)>& in_use,
                 std::string* save_error);
  // Pin a board to an ID (replaces any other board's claim on it).
  bool pin(uint64_t mac, uint8_t node_id, uint16_t type_id, std::string* error);
  bool forget(uint8_t node_id, std::string* error);  // false if no board holds that ID

  const Assignment* find_by_node(uint8_t node_id) const;
  const Assignment* find_by_mac(uint64_t mac) const;
  std::vector<Assignment> table() const;
  uint8_t first() const { return first_; }
  uint8_t last() const { return last_; }

  static std::string mac_to_string(uint64_t mac);  // "a4:cf:12:34:56:78"
  static uint64_t mac_from_bytes(const uint8_t* bytes6);
  static void mac_to_bytes(uint64_t mac, uint8_t* bytes6);

 private:
  bool save(std::string* error) const;
  std::string path_;
  uint8_t first_, last_;
  std::map<uint64_t, Assignment> by_mac_;
};

}  // namespace anv::host
