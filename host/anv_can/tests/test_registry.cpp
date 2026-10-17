// Node registry and ID allocator, with a fake clock and a temporary table file.
#include <cstdio>
#include <fstream>

#include <unistd.h>

#include "anv/host/allocator.hpp"
#include "anv/host/registry.hpp"
#include "check.hpp"

using namespace anv;
using namespace anv::host;

namespace {

constexpr int64_t kMs = 1'000'000;

NodeHeartbeat hb_for(uint8_t src, const NodeType& t, uint16_t uptime = 1, uint8_t state = 2) {
  NodeHeartbeat h;
  h.src = src;
  h.type_id = t.type_id;
  h.schema_hash = t.schema_hash16();
  h.uptime_s = uptime;
  h.state = state;
  return h;
}

size_t count(const std::vector<RegistryEvent>& ev, RegistryEvent::Kind k) {
  size_t n = 0;
  for (const auto& e : ev) n += e.kind == k;
  return n;
}

std::string tmp_path(const char* name) {
  return "/tmp/anv_host_test_" + std::to_string(getpid()) + "_" + name;
}

}  // namespace

TEST(discovers_and_checks_schema) {
  const Schema& s = t::schema();
  NodeRegistry r(s);
  std::vector<RegistryEvent> ev;
  // Whatever types the schema has: the registry learns them from type_id, nothing is listed.
  for (size_t i = 0; i < s.types.size(); ++i) r.on_heartbeat(hb_for(static_cast<uint8_t>(10 + i), s.types[i]), 0, ev);
  CHECK(count(ev, RegistryEvent::Kind::Appeared) == s.types.size());
  for (size_t i = 0; i < s.types.size(); ++i) {
    const NodeInfo* n = r.find(static_cast<uint8_t>(10 + i));
    CHECK(n && n->online && n->compat == Compat::Ok && n->usable_type() == &s.types[i]);
  }
  NodeHeartbeat bad = hb_for(40, s.types[0]);
  bad.schema_hash ^= 1;
  r.on_heartbeat(bad, 0, ev);
  CHECK(r.find(40)->compat == Compat::SchemaMismatch && r.find(40)->usable_type() == nullptr &&
        r.find(40)->type == &s.types[0]);
  NodeHeartbeat unknown;
  unknown.src = 41;
  unknown.type_id = 0xFFF0;
  r.on_heartbeat(unknown, 0, ev);
  CHECK(r.find(41)->compat == Compat::UnknownType && r.find(41)->type == nullptr);
}

TEST(online_offline_reboot_state) {
  const Schema& s = t::schema();
  const NodeType& nt = s.types[0];
  NodeRegistry r(s);
  std::vector<RegistryEvent> ev;
  for (int i = 0; i < 10; ++i) r.on_heartbeat(hb_for(20, nt, static_cast<uint16_t>(5 + i)), i * 100 * kMs, ev);
  CHECK(r.find(20)->heartbeat_hz > 9 && r.find(20)->heartbeat_hz < 11);
  ev.clear();
  r.tick(1300 * kMs, ev);
  CHECK(r.online(20) && ev.empty());  // 3.5 periods of a 10 Hz heartbeat = 350 ms < 500 ms default
  r.tick(1500 * kMs, ev);
  CHECK(!r.online(20) && count(ev, RegistryEvent::Kind::Lost) == 1);
  r.tick(1600 * kMs, ev);
  CHECK(count(ev, RegistryEvent::Kind::Lost) == 1);  // once
  ev.clear();
  r.on_heartbeat(hb_for(20, nt, 1), 2000 * kMs, ev);  // back, and its uptime restarted
  CHECK(r.online(20) && ev.size() == 2 && count(ev, RegistryEvent::Kind::Rebooted) == 1);
  CHECK(ev.back().kind == RegistryEvent::Kind::Appeared && ev.back().returned && r.find(20)->reboots == 1);
  ev.clear();
  r.on_heartbeat(hb_for(20, nt, 2, 3), 2100 * kMs, ev);
  CHECK(ev.size() == 1 && ev[0].kind == RegistryEvent::Kind::StateChanged && ev[0].old_state == 2);
}

TEST(slow_heartbeat_doesnt_flap) {
  const Schema& s = t::schema();
  NodeRegistry r(s);
  std::vector<RegistryEvent> ev;
  for (int i = 0; i < 5; ++i) r.on_heartbeat(hb_for(20, s.types[0], static_cast<uint16_t>(i)), i * 1000 * kMs, ev);
  r.tick(4000 * kMs + 2000 * kMs, ev);  // 2 s after the last 1 Hz heartbeat: still online
  CHECK(r.online(20));
  r.tick(4000 * kMs + 4000 * kMs, ev);
  CHECK(!r.online(20));
}

TEST(allocator_same_board_same_id) {
  const std::string path = tmp_path("ids.txt");
  std::remove(path.c_str());
  IdAllocator a(10, 12, path);
  std::string err;
  CHECK(a.load(&err));
  auto none = [](uint8_t) { return false; };
  CHECK(a.assign(0xA4CF12345678ull, 2, none, &err) == 10);
  CHECK(a.assign(0xA4CF12345679ull, 2, [](uint8_t id) { return id == 11; }, &err) == 12);  // 11 is taken
  CHECK(a.assign(0xA4CF12345678ull, 2, none, &err) == 10);  // asked again: same answer
  CHECK(a.assign(0x1ull, 3, none, &err) == 11);
  CHECK(a.assign(0x2ull, 3, [](uint8_t) { return true; }, &err) == 0);  // everything in use
  // Range full of remembered boards: a new board takes the ID of one that's not on the bus.
  CHECK(a.assign(0x2ull, 3, [](uint8_t id) { return id != 12; }, &err) == 12);
  CHECK(a.find_by_mac(0xA4CF12345679ull) == nullptr);

  IdAllocator b(10, 12, path);  // the table survives a restart
  CHECK(b.load(&err) && b.table().size() == 3);
  CHECK(b.find_by_node(10) && b.find_by_node(10)->mac == 0xA4CF12345678ull && b.find_by_node(10)->type_id == 2);
  CHECK(b.forget(10, &err) && !b.forget(10, &err));
  CHECK(b.pin(0x3ull, 11, 4, &err) && b.find_by_node(11)->mac == 0x3ull && b.find_by_mac(0x1ull) == nullptr);
  // A remembered ID that another board is using now: give a different one.
  CHECK(b.assign(0x3ull, 4, [](uint8_t id) { return id == 11; }, &err) == 10);
  std::remove(path.c_str());
}

TEST(allocator_table_errors) {
  const std::string path = tmp_path("bad.txt");
  {
    std::ofstream f(path);
    f << "# comment\n\na4cf12345678 20 2\nnot a line\n";
  }
  IdAllocator a(1, 254, path);
  std::string err;
  CHECK(!a.load(&err) && err.find(":4:") != std::string::npos);
  std::remove(path.c_str());
  IdAllocator missing(1, 254, tmp_path("missing.txt"));
  CHECK(missing.load(&err) && missing.table().empty());
}

TEST(mac_text) {
  uint8_t b[6] = {0xa4, 0xcf, 0x12, 0x34, 0x56, 0x78};
  const uint64_t m = IdAllocator::mac_from_bytes(b);
  CHECK(m == 0xA4CF12345678ull && IdAllocator::mac_to_string(m) == "a4:cf:12:34:56:78");
  uint8_t back[6];
  IdAllocator::mac_to_bytes(m, back);
  CHECK(back[0] == 0xa4 && back[5] == 0x78);
}

TEST_MAIN()
