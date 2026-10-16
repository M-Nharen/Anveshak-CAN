#include "anv_gateway/id_allocator.hpp"

#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <sstream>

#include "anv_gateway/protocol.hpp"

namespace anv::gw {

namespace {
constexpr uint64_t kMacMask = (uint64_t{1} << 48) - 1;
}

IdAllocator::IdAllocator(std::string table_path) : path_(std::move(table_path)) {}

std::vector<uint8_t> IdAllocator::pool() {
  std::vector<uint8_t> p;
  for (uint8_t id = kFirstAuxNode; id <= kLastAuxNode; ++id)
    if (id != kNodeFpga && id != kNodePower) p.push_back(id);  // reserved IDs never go to an unknown board
  return p;
}

std::string IdAllocator::mac_to_string(uint64_t mac) {
  char buf[18];
  std::snprintf(buf, sizeof buf, "%02x:%02x:%02x:%02x:%02x:%02x", static_cast<unsigned>((mac >> 40) & 0xFF),
                static_cast<unsigned>((mac >> 32) & 0xFF), static_cast<unsigned>((mac >> 24) & 0xFF),
                static_cast<unsigned>((mac >> 16) & 0xFF), static_cast<unsigned>((mac >> 8) & 0xFF),
                static_cast<unsigned>(mac & 0xFF));
  return buf;
}

bool IdAllocator::load(std::string* error) {
  by_mac_.clear();
  if (path_.empty()) return true;
  std::ifstream f(path_);
  if (!f) return true;
  std::string line;
  int line_no = 0;
  while (std::getline(f, line)) {
    ++line_no;
    const size_t hash = line.find('#');
    if (hash != std::string::npos) line.resize(hash);
    std::istringstream in(line);
    std::string mac_hex;
    unsigned id = 0, hw = 0xFF;
    if (!(in >> mac_hex)) continue;  // blank line
    if (!(in >> id)) {
      if (error) *error = path_ + " line " + std::to_string(line_no) + ": expected '<mac> <node id> [hw target]'";
      return false;
    }
    in >> hw;
    char* end = nullptr;
    const unsigned long long mac = std::strtoull(mac_hex.c_str(), &end, 16);
    if (end == nullptr || *end != '\0' || id > 31) {
      if (error) *error = path_ + " line " + std::to_string(line_no) + ": bad MAC or node ID";
      return false;
    }
    IdAssignment a;
    a.mac = static_cast<uint64_t>(mac) & kMacMask;
    a.node_id = static_cast<uint8_t>(id);
    a.hw_target = static_cast<uint8_t>(hw);
    by_mac_[a.mac] = a;
  }
  return true;
}

bool IdAllocator::save(std::string* error) const {
  if (path_.empty()) return true;
  // Write a temporary file and rename it over the table, so a power cut never leaves half a table.
  const std::string tmp = path_ + ".tmp";
  {
    std::ofstream f(tmp, std::ios::trunc);
    if (!f) {
      if (error) *error = "cannot write " + tmp;
      return false;
    }
    f << "# Node ID table written by anv_can_gateway: <mac> <node id> <hw target 0 ESP32, 1 ESP32-S3>\n";
    for (const auto& [mac, a] : by_mac_) {
      char hex[13];
      std::snprintf(hex, sizeof hex, "%012llx", static_cast<unsigned long long>(mac));
      f << hex << ' ' << static_cast<unsigned>(a.node_id) << ' ' << static_cast<unsigned>(a.hw_target) << '\n';
    }
    if (!f) {
      if (error) *error = "write to " + tmp + " failed";
      return false;
    }
  }
  if (std::rename(tmp.c_str(), path_.c_str()) != 0) {
    if (error) *error = "cannot replace " + path_;
    return false;
  }
  return true;
}

uint8_t IdAllocator::assign(uint64_t mac, uint8_t hw_target, const std::function<bool(uint8_t)>& in_use,
                            std::string* save_error) {
  mac &= kMacMask;
  auto it = by_mac_.find(mac);
  if (it != by_mac_.end()) {
    if (it->second.hw_target != hw_target) {
      it->second.hw_target = hw_target;
      save(save_error);
    }
    return it->second.node_id;
  }
  for (uint8_t id : pool()) {
    if (find_by_node(id) || (in_use && in_use(id))) continue;
    by_mac_[mac] = IdAssignment{mac, id, hw_target};
    save(save_error);
    return id;
  }
  return 0;
}

bool IdAllocator::forget_node(uint8_t node_id, std::string* save_error) {
  for (auto it = by_mac_.begin(); it != by_mac_.end(); ++it) {
    if (it->second.node_id == node_id) {
      by_mac_.erase(it);
      save(save_error);
      return true;
    }
  }
  return false;
}

const IdAssignment* IdAllocator::find_by_node(uint8_t node_id) const {
  for (const auto& [mac, a] : by_mac_)
    if (a.node_id == node_id) return &a;
  return nullptr;
}

std::vector<IdAssignment> IdAllocator::table() const {
  std::vector<IdAssignment> out;
  for (const auto& [mac, a] : by_mac_) out.push_back(a);
  return out;
}

}  // namespace anv::gw
