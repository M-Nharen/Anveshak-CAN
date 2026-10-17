// anv_host end to end on the in-process test bus, against fake nodes (fake_node.hpp). Types are
// picked from the schema by name; the host itself is told nothing about them.
#include <chrono>
#include <cstdio>
#include <cstring>
#include <thread>

#include <unistd.h>

#include "anv/host/host.hpp"
#include "check.hpp"
#include "fake_node.hpp"

using namespace anv;
using namespace anv::host;
using namespace std::chrono_literals;

namespace {

std::shared_ptr<const Schema> host_schema() {
  std::string err;
  return Schema::load_file(Schema::default_path(), &err);
}

HostConfig config(uint8_t id) {
  HostConfig c;
  c.host_id = id;
  c.allocator = HostConfig::Allocator::Off;
  c.node_timeout_ms = 300;
  c.service_timeout_ms = 200;
  return c;
}

const NodeType& type(const char* name) { return *t::schema().type(name); }

// Poll until cond() or the timeout (the bus is real threads, so things take a few ms).
template <class F>
bool eventually(F cond, int ms = 2000) {
  const auto end = std::chrono::steady_clock::now() + std::chrono::milliseconds(ms);
  while (std::chrono::steady_clock::now() < end) {
    if (cond()) return true;
    std::this_thread::sleep_for(2ms);
  }
  return cond();
}

uint32_t f32_bits(float f) {
  uint32_t u;
  std::memcpy(&u, &f, 4);
  return u;
}

}  // namespace

TEST(value_wire_format) {
  ValueType t;
  uint32_t w = 0;
  CHECK(ValueType::parse("f32", t) && value_to_wire(t, 1.5, w, nullptr) && w == f32_bits(1.5f));
  CHECK(value_from_wire(t, w) == 1.5);
  CHECK(ValueType::parse("i16", t) && value_to_wire(t, -2, w, nullptr) && w == 0xFFFFFFFEu);
  CHECK(value_from_wire(t, w) == -2 && value_from_wire(t, 0xFFFEu) == -2);
  CHECK(!value_to_wire(t, 40000, w, nullptr));
  CHECK(ValueType::parse("u8", t) && value_to_wire(t, 254.6, w, nullptr) && w == 255);
  CHECK(!value_to_wire(t, -1, w, nullptr) && !value_to_wire(t, 256, w, nullptr));
  CHECK(ValueType::parse("bool", t) && value_to_wire(t, 1, w, nullptr) && w == 1 && !value_to_wire(t, 2, w, nullptr));
  CHECK(ValueType::parse("u32", t) && value_to_wire(t, 4294967295.0, w, nullptr) && w == 0xFFFFFFFFu);
}

TEST(discovers_nodes_and_checks_schema) {
  TestBus bus;
  auto link = bus.connect();
  Host h(*link, host_schema(), config(1));
  std::atomic<int> appeared{0}, lost{0};
  HostCallbacks cb;
  cb.node_appeared = [&](const NodeInfo&, bool) { ++appeared; };
  cb.node_lost = [&](const NodeInfo& n) { lost += n.id == 20; };
  CHECK(h.set_callbacks(cb));
  CHECK(h.start());
  CHECK(!h.set_callbacks(cb));  // not while running

  t::FakeNode a(bus, type("esp32_bench"), 20), b(bus, type("motor_controller"), 21),
      c(bus, type("motor_controller"), 22);
  c.bad_hash = true;
  a.start();
  b.start();
  c.start();
  CHECK(h.wait_for_node(20, 1000) && h.wait_for_node(21, 1000) && h.wait_for_node(22, 1000));
  CHECK(eventually([&] { return appeared == 3; }));
  auto n20 = h.node(20), n21 = h.node(21), n22 = h.node(22);
  CHECK(n20 && n20->compat == Compat::Ok && n20->type && n20->type->name == "esp32_bench");
  CHECK(n21 && n21->compat == Compat::Ok && n21->type->name == "motor_controller");
  CHECK(n22 && n22->compat == Compat::SchemaMismatch);
  CHECK(h.nodes().size() == 3);
  // Nothing is decoded or asked of a node built from a different schema.
  CHECK(h.get(22, "kp_0").status == Status::NoSuchNode);

  a.stop();
  CHECK(eventually([&] { return lost == 1; }));
  CHECK(!h.node(20)->online && h.node(21)->online);
  CHECK(h.get(20, "heartbeat_hz").status == Status::NoSuchNode);
  h.stop();
}

TEST(assigns_ids_and_remembers_them) {
  const std::string table = "/tmp/anv_host_test_assign_" + std::to_string(getpid()) + ".txt";
  std::remove(table.c_str());
  TestBus bus;
  auto link = bus.connect();
  HostConfig cfg = config(1);
  cfg.allocator = HostConfig::Allocator::Auto;  // nobody else is around, so this host answers
  cfg.id_first = 30;
  cfg.id_last = 40;
  cfg.id_table_path = table;
  std::atomic<int> assigned{0};
  {
    Host h(*link, host_schema(), cfg);
    HostCallbacks cb;
    cb.id_assigned = [&](uint8_t, uint64_t, uint16_t) { ++assigned; };
    h.set_callbacks(cb);
    CHECK(h.start());
    t::FakeNode a(bus, type("esp32_bench"), 0, 0xA4CF12000001ull), b(bus, type("esp32_bench"), 0, 0xA4CF12000002ull);
    a.start();
    CHECK(eventually([&] { return a.id_ != 0; }));
    b.start();
    CHECK(eventually([&] { return b.id_ != 0; }));
    CHECK(a.id_ == 30 && b.id_ == 31);
    CHECK(h.wait_for_node(30, 1000) && h.node(30)->mac == 0xA4CF12000001ull);
    CHECK(assigned >= 2);
    h.stop();
  }
  // Restart: the same board gets the same ID back, even asking first.
  Host h2(*link, host_schema(), cfg);
  CHECK(h2.start());
  CHECK(h2.id_table().size() == 2);
  t::FakeNode b(bus, type("esp32_bench"), 0, 0xA4CF12000002ull);
  b.start();
  CHECK(eventually([&] { return b.id_ != 0; }) && b.id_ == 31);
  h2.stop();
  std::remove(table.c_str());
}

TEST(allocator_off_and_observer_never_answer) {
  TestBus bus;
  auto l1 = bus.connect(), l2 = bus.connect();
  Host off(*l1, host_schema(), config(1));
  HostConfig oc = config(2);
  oc.observer = true;
  Host obs(*l2, host_schema(), oc);
  CHECK(off.start() && obs.start());
  t::FakeNode a(bus, type("esp32_bench"), 0, 0x42);
  a.start();
  std::this_thread::sleep_for(200ms);
  CHECK(a.id_ == 0);
}

TEST(only_the_lowest_host_allocates_without_a_leader) {
  TestBus bus;
  auto l1 = bus.connect(), l2 = bus.connect();
  HostConfig c1 = config(5), c2 = config(3);
  c1.allocator = c2.allocator = HostConfig::Allocator::Auto;
  c1.id_first = c1.id_last = 50;  // different tables: whichever answers is visible in the ID
  c2.id_first = c2.id_last = 60;
  Host h5(*l1, host_schema(), c1), h3(*l2, host_schema(), c2);
  CHECK(h5.start() && h3.start());
  CHECK(eventually([&] { return h5.hosts().size() == 1 && h3.hosts().size() == 1; }));
  t::FakeNode a(bus, type("esp32_bench"), 0, 0x77);
  a.start();
  CHECK(eventually([&] { return a.id_ != 0; }) && a.id_ == 60);
  // Once host 5 leads, it is the one that answers.
  a.stop();
  CHECK(h5.request_lead() == Status::Ok && h5.leading());
  CHECK(eventually([&] { return h3.leader() == 5; }));
  t::FakeNode b(bus, type("esp32_bench"), 0, 0x78);
  b.start();
  CHECK(eventually([&] { return b.id_ != 0; }) && b.id_ == 50);
}

TEST(settings) {
  TestBus bus;
  auto l1 = bus.connect(), l2 = bus.connect();
  Host h(*l1, host_schema(), config(1));
  HostConfig oc = config(2);
  oc.observer = true;
  Host watcher(*l2, host_schema(), oc);
  std::atomic<int> changes{0};
  std::atomic<double> changed_to{0};
  HostCallbacks cb;
  cb.setting_changed = [&](uint8_t node, const Param& p, double v) {
    if (node == 21 && p.name == "kp_0") {
      changed_to = v;
      ++changes;
    }
  };
  watcher.set_callbacks(cb);
  CHECK(h.start() && watcher.start());
  t::FakeNode n(bus, type("motor_controller"), 21);
  n.start();
  CHECK(h.wait_for_node(21, 1000));

  const NodeType& mc = type("motor_controller");
  const Param* kp0 = mc.param("kp_0");
  Result r = h.get(21, "kp_0");
  CHECK(r.ok() && r.value == *kp0->def);
  r = h.set(21, "kp_0", 12.5);
  CHECK(r.ok() && r.value == 12.5 && n.param(kp0->index) == f32_bits(12.5f));
  CHECK(h.get(21, "kp_0").value == 12.5);
  // Another host sees the change.
  CHECK(eventually([&] { return changes == 1; }) && changed_to == 12.5);

  // Checked here before anything goes on the bus.
  CHECK(h.set(21, "kp_0", 5000).status == Status::OutOfRange);
  CHECK(h.set(21, "total_runtime_s", 1).status == Status::ReadOnly);
  CHECK(h.get(21, "no_such_param").status == Status::UnknownName);
  CHECK(h.set(21, "invert_0", 2).status == Status::BadValue);

  // The protocol's common parameters work on every node, by name or index.
  const Param* hb = t::schema().protocol.param("heartbeat_hz");
  CHECK(h.get(21, "heartbeat_hz").value == *hb->def);
  char idx[16];
  std::snprintf(idx, sizeof idx, "0x%04X", hb->index);
  CHECK(h.set(21, idx, 20).ok() && h.get(21, "heartbeat_hz").value == 20);

  CHECK(h.save(21).ok() && n.saves == 1);
  CHECK(h.get(99, "kp_0").status == Status::NoSuchNode);
}

TEST(stream_rates_and_telemetry) {
  TestBus bus;
  auto link = bus.connect();
  Host h(*link, host_schema(), config(1));
  std::atomic<int> feedback{0};
  HostCallbacks cb;
  cb.telemetry = [&](uint8_t node, const Message& m, const Values& v, int64_t) {
    if (node == 21 && m.name == "feedback" && v.count("speed")) ++feedback;
  };
  h.set_callbacks(cb);
  CHECK(h.start());
  t::FakeNode n(bus, type("motor_controller"), 21);
  n.start();
  CHECK(h.wait_for_node(21, 1000));
  const Message* fb = type("motor_controller").message("feedback");
  CHECK(h.get_rate(21, "feedback").value == fb->rate->def);
  CHECK(eventually([&] { return feedback > 5; }));

  Result r = h.set_rate(21, "feedback", 2.5);
  CHECK(r.ok() && r.value == 2.5 && n.rate_mhz(fb->port) == 2500);
  CHECK(h.get_rate(21, "feedback").value == 2.5);
  CHECK(h.set_rate(21, "feedback", fb->rate->max + 1).status == Status::OutOfRange);
  CHECK(h.set_rate(21, "setpoint", 10).status == Status::UnknownName);  // a command, not a stream
  CHECK(h.set_rate(21, "feedback", 0).ok() && n.rate_mhz(fb->port) == 0);
  std::this_thread::sleep_for(50ms);
  const int before = feedback;
  std::this_thread::sleep_for(150ms);
  CHECK(feedback == before);
}

TEST(actions) {
  TestBus bus;
  auto link = bus.connect();
  Host h(*link, host_schema(), config(1));
  std::atomic<int> from_callback{-1};
  HostCallbacks cb;
  cb.node_appeared = [&](const NodeInfo& n, bool) { from_callback = static_cast<int>(h.action(n.id, "blink", 1).status); };
  h.set_callbacks(cb);
  CHECK(h.start());
  t::FakeNode n(bus, type("esp32_bench"), 20);
  n.start();
  CHECK(h.wait_for_node(20, 1000));
  CHECK(eventually([&] { return from_callback >= 0; }) && from_callback == static_cast<int>(Status::WouldDeadlock));
  CHECK(h.action(20, "blink", 5).ok() && n.actions == 1 && n.last_action_arg == 5);
  CHECK(h.action(20, "blink").status == Status::BadValue);
  CHECK(h.action(20, "blink", 500).status == Status::BadValue);  // over u8
  CHECK(h.action(20, "nope").status == Status::UnknownName);
}

TEST(leader_arms_commands_and_watchdog) {
  TestBus bus;
  auto l1 = bus.connect(), l2 = bus.connect();
  Host a(*l1, host_schema(), config(1)), b(*l2, host_schema(), config(2));
  CHECK(a.start() && b.start());
  t::FakeNode n(bus, type("motor_controller"), 21);
  n.start();
  CHECK(a.wait_for_node(21, 1000) && b.wait_for_node(21, 1000));

  Values sp;
  sp["mode"] = one(std::string("velocity"));
  sp["target"] = FieldValue{1.0, -1.0};
  // Nobody leads yet: nothing moves.
  CHECK(a.leader() == 0 && a.arm(21) == Status::NotLeader);
  CHECK(a.set_command(21, "setpoint", sp).status == Status::NotLeader);

  CHECK(a.request_lead() == Status::Ok && a.leading() && a.term() == 1);
  CHECK(eventually([&] { return b.leader() == 1 && n.leader == 1; }));
  CHECK(a.arm(21) == Status::Ok);
  CHECK(eventually([&] { return n.armed.load(); }));
  // Without commands the node's watchdog disarms it.
  CHECK(eventually([&] { return !n.armed; }, 1000));

  CHECK(a.arm(21) == Status::Ok && eventually([&] { return n.armed.load(); }));
  CHECK(a.set_command(21, "setpoint", sp, 50).ok());
  CHECK(a.set_command(21, "feedback", sp).status == Status::UnknownName);  // not a command
  Values bad;
  bad["nope"] = one(int64_t{1});
  CHECK(a.set_command(21, "setpoint", bad).status == Status::BadValue);
  std::this_thread::sleep_for(400ms);
  CHECK(n.armed && n.commands >= 10);  // refreshed: the watchdog never fires
  a.stop_command(21, "setpoint");
  CHECK(eventually([&] { return !n.armed; }, 1000));

  // The operator hands the lead to the other host: new term, the node disarms, the old
  // leader's refreshed commands stop.
  CHECK(a.arm(21) == Status::Ok && a.set_command(21, "setpoint", sp).ok());
  CHECK(eventually([&] { return n.armed.load(); }));
  CHECK(a.request_lead(2) == Status::Ok);
  CHECK(eventually([&] { return b.leading() && !a.leading() && a.leader() == 2; }));
  CHECK(b.term() == 2 && n.leader == 2);
  CHECK(eventually([&] { return !n.armed; }, 500));
  CHECK(a.set_command(21, "setpoint", sp).status == Status::NotLeader);
  const int count = n.commands;
  std::this_thread::sleep_for(100ms);
  CHECK(n.commands == count);
}

TEST(estop_from_anyone) {
  TestBus bus;
  auto l1 = bus.connect(), l2 = bus.connect();
  Host a(*l1, host_schema(), config(1)), b(*l2, host_schema(), config(2));
  std::atomic<int> seen{0};
  HostCallbacks cb;
  cb.estop = [&](uint8_t src, uint8_t reason) { seen += src == 2 && reason == 7; };
  a.set_callbacks(cb);
  CHECK(a.start() && b.start());
  t::FakeNode n(bus, type("motor_controller"), 21);
  n.start();
  CHECK(a.wait_for_node(21, 1000));
  Values sp;
  sp["mode"] = one(std::string("velocity"));
  CHECK(a.request_lead() == Status::Ok && eventually([&] { return n.leader == 1; }));
  CHECK(a.arm(21) == Status::Ok && a.set_command(21, "setpoint", sp).ok());
  CHECK(eventually([&] { return n.armed.load(); }));

  CHECK(b.estop(7) == Status::Ok);  // the standby host, not the leader
  CHECK(eventually([&] { return n.estop && !n.armed; }));
  CHECK(eventually([&] { return seen == 1 && a.estop_latched(); }));
  CHECK(a.arm(21) == Status::Ok);  // the node refuses while latched
  std::this_thread::sleep_for(100ms);
  CHECK(!n.armed);
  CHECK(b.estop_release() == Status::NotLeader);
  CHECK(a.estop_release() == Status::Ok && eventually([&] { return !n.estop; }));
  CHECK(!a.estop_latched());
}

TEST(not_running) {
  TestBus bus;
  auto link = bus.connect();
  Host h(*link, host_schema(), config(1));
  CHECK(h.get(1, "x").status == Status::NotRunning);
  CHECK(h.request_lead() == Status::NotRunning);
  HostConfig bad = config(0);
  Host b(*link, host_schema(), bad);
  std::string err;
  CHECK(!b.start(&err) && !err.empty());
}

TEST_MAIN()
