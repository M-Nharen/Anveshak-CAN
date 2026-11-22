// Node registry, with a fake clock.
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

TEST_MAIN()
