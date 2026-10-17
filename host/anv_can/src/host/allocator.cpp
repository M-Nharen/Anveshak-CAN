#include "anv/host/allocator.hpp"

#include <cstdio>
#include <fstream>
#include <sstream>

namespace anv::host {

IdAllocator::IdAllocator(uint8_t first, uint8_t last, std::string table_path)
    : path_(std::move(table_path)), first_(first), last_(last) {}

bool IdAllocator::load(std::string* error) {
  by_mac_.clear();
  if (path_.empty()) return true;
  std::ifstream in(path_);
  if (!in) return true;
  std::string line;
  int line_no = 0;
  while (std::getline(in, line)) {
    ++line_no;
    const size_t hash = line.find('#');
    if (hash != std::string::npos) line.resize(hash);
    std::istringstream ls(line);
    std::string mac_text;
    unsigned id = 0, type_id = 0;
    if (!(ls >> mac_text)) continue;  // blank
    if (!(ls >> id >> type_id) || mac_text.size() != 12 || id == 0 || id > 255 || type_id > 0xFFFF) {
      if (error) *error = path_ + ":" + std::to_string(line_no) + ": expected <12 hex digit MAC> <id> <type_id>";
      return false;
    }
    Assignment a;
    a.mac = std::stoull(mac_text, nullptr, 16);
    a.node_id = static_cast<uint8_t>(id);
    a.type_id = static_cast<uint16_t>(type_id);
    by_mac_[a.mac] = a;
  }
  return true;
}

bool IdAllocator::save(std::string* error) const {
  if (path_.empty()) return true;
  const std::string tmp = path_ + ".tmp";
  {
    std::ofstream out(tmp, std::ios::trunc);
    if (!out) {
      if (error) *error = "cannot write " + tmp;
      return false;
    }
    out << "# anv_host node ID table: MAC node_id type_id\n";
    char buf[64];
    for (const auto& kv : by_mac_) {
      std::snprintf(buf, sizeof buf, "%012llx %u %u\n", static_cast<unsigned long long>(kv.first),
                    kv.second.node_id, kv.second.type_id);
      out << buf;
    }
    if (!out) {
      if (error) *error = "cannot write " + tmp;
      return false;
    }
  }
  // Rename so a crash mid-write never leaves a half-written table.
  if (std::rename(tmp.c_str(), path_.c_str()) != 0) {
    if (error) *error = "cannot replace " + path_;
    return false;
  }
  return true;
}

uint8_t IdAllocator::assign(uint64_t mac, uint16_t type_id, const std::function<bool(uint8_t)>& in_use,
                            std::string* save_error) {
  auto used = [&](unsigned id) { return in_use && in_use(static_cast<uint8_t>(id)); };
  auto held_by_other = [&](unsigned id) {
    const Assignment* a = find_by_node(static_cast<uint8_t>(id));
    return a != nullptr && a->mac != mac;
  };
  auto it = by_mac_.find(mac);
  if (it != by_mac_.end() && !used(it->second.node_id)) {
    if (it->second.type_id != type_id) {
      it->second.type_id = type_id;
      save(save_error);
    }
    return it->second.node_id;
  }
  unsigned chosen = 0;
  for (unsigned id = first_; id <= last_ && chosen == 0; ++id) {
    if (!used(id) && !held_by_other(id)) chosen = id;
  }
  // Range full of remembered boards: reuse the ID of one that isn't on the bus.
  for (unsigned id = first_; id <= last_ && chosen == 0; ++id) {
    if (!used(id)) chosen = id;
  }
  if (chosen == 0) return 0;
  for (auto o = by_mac_.begin(); o != by_mac_.end();) {
    o = o->second.node_id == chosen ? by_mac_.erase(o) : std::next(o);
  }
  by_mac_[mac] = Assignment{mac, static_cast<uint8_t>(chosen), type_id};
  save(save_error);
  return static_cast<uint8_t>(chosen);
}

bool IdAllocator::pin(uint64_t mac, uint8_t node_id, uint16_t type_id, std::string* error) {
  for (auto o = by_mac_.begin(); o != by_mac_.end();) {
    o = o->second.node_id == node_id ? by_mac_.erase(o) : std::next(o);
  }
  by_mac_[mac] = Assignment{mac, node_id, type_id};
  return save(error);
}

bool IdAllocator::forget(uint8_t node_id, std::string* error) {
  for (auto o = by_mac_.begin(); o != by_mac_.end(); ++o) {
    if (o->second.node_id == node_id) {
      by_mac_.erase(o);
      return save(error);
    }
  }
  return false;
}

const Assignment* IdAllocator::find_by_node(uint8_t node_id) const {
  for (const auto& kv : by_mac_) {
    if (kv.second.node_id == node_id) return &kv.second;
  }
  return nullptr;
}

const Assignment* IdAllocator::find_by_mac(uint64_t mac) const {
  auto it = by_mac_.find(mac);
  return it == by_mac_.end() ? nullptr : &it->second;
}

std::vector<Assignment> IdAllocator::table() const {
  std::vector<Assignment> out;
  for (const auto& kv : by_mac_) out.push_back(kv.second);
  return out;
}

std::string IdAllocator::mac_to_string(uint64_t mac) {
  char buf[24];
  std::snprintf(buf, sizeof buf, "%02x:%02x:%02x:%02x:%02x:%02x", static_cast<unsigned>((mac >> 40) & 0xFF),
                static_cast<unsigned>((mac >> 32) & 0xFF), static_cast<unsigned>((mac >> 24) & 0xFF),
                static_cast<unsigned>((mac >> 16) & 0xFF), static_cast<unsigned>((mac >> 8) & 0xFF),
                static_cast<unsigned>(mac & 0xFF));
  return buf;
}

// mac[0] is the first byte people write (a4 in a4:cf:...), as in id_request.
uint64_t IdAllocator::mac_from_bytes(const uint8_t* b) {
  uint64_t m = 0;
  for (int i = 0; i < 6; ++i) m = (m << 8) | b[i];
  return m;
}

void IdAllocator::mac_to_bytes(uint64_t mac, uint8_t* b) {
  for (int i = 5; i >= 0; --i) {
    b[i] = static_cast<uint8_t>(mac & 0xFF);
    mac >>= 8;
  }
}

}  // namespace anv::host
