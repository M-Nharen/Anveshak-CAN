// anv_proto against the generated C (what the ESP32 firmware compiles): IDs and frames must be
// byte-identical. The Python tests extend this to every message with random vectors.
#include <cmath>
#include <cstring>
#include <random>

#include "anv/codec.hpp"
#include "anv_all_types.h"
#include "anv_motor_controller.h"
#include "anv_protocol.h"
#include "check.hpp"

using namespace anv;

namespace {

std::vector<uint8_t> enc(const Message& m, const Values& v, bool raw = true) {
  std::vector<uint8_t> out;
  std::string err;
  EncodeOptions o;
  o.raw = raw;
  if (!encode(m, v, out, &err, o)) std::printf("  encode %s: %s\n", m.name.c_str(), err.c_str());
  return out;
}

bool same(const std::vector<uint8_t>& a, const uint8_t* b, size_t n) {
  return a.size() == n && std::memcmp(a.data(), b, n) == 0;
}

const Message& pmsg(const char* name) { return *t::schema().protocol.message(name); }

}  // namespace

TEST(schema_loads) {
  const Schema& s = t::schema();
  CHECK(s.protocol.name == "protocol");
  CHECK(s.protocol_last_port == ANV_PROTOCOL_LAST_PORT);
  CHECK(s.max_port() == ANV_MAX_PORT);
  CHECK(s.node_ids.unassigned == ANV_NODE_UNASSIGNED);
  CHECK(s.protocol.wire_hash == anv_protocol_type.wire_hash);
  CHECK(s.protocol.messages.size() == anv_protocol_type.n_messages);
  // Every type the firmware knows, the host knows, with the same hash.
  for (size_t i = 0; anv_all_types[i] != nullptr; ++i) {
    const anv_type_desc_t* c = anv_all_types[i];
    const NodeType* t = s.type_by_id(c->type_id);
    CHECK(t != nullptr);
    if (!t) continue;
    CHECK(t->name == c->name);
    CHECK(t->wire_hash == c->wire_hash);
    CHECK(t->messages.size() == c->n_messages);
    for (uint16_t k = 0; k < c->n_messages; ++k) {
      const Message* m = t->message(c->messages[k].port);
      CHECK(m != nullptr && m->length == c->messages[k].length && m->priority == c->messages[k].priority);
    }
  }
  CHECK(s.type("protocol") == &s.protocol);
  CHECK(s.type("no_such_type") == nullptr);
}

TEST(schema_rejects_bad_json) {
  std::string err;
  CHECK(Schema::load_json("{", &err) == nullptr && !err.empty());
  CHECK(Schema::load_json("{\"format\": 99}", &err) == nullptr && err.find("format") != std::string::npos);
  CHECK(Schema::load_file("/nonexistent/schema.json", &err) == nullptr);
}

TEST(ids_match_generated_c) {
  const IdLayout& l = t::schema().id_layout;
  std::mt19937 rng(7);
  for (int i = 0; i < 20000; ++i) {
    const uint32_t pr = rng() % 8, dst = rng() % 256, port = rng() % 1024, src = rng() % 256;
    const uint32_t id = l.make(pr, dst, port, src);
    CHECK(id == anv_id_make(static_cast<uint8_t>(pr), static_cast<uint8_t>(dst), static_cast<uint16_t>(port),
                            static_cast<uint8_t>(src)));
    const IdParts p = l.parse(id);
    CHECK(p.priority == pr && p.dst == dst && p.port == port && p.src == src);
    CHECK(p.dst == anv_id_dst(id) && p.priority == anv_id_priority(id));
  }
  uint32_t id;
  std::string err;
  CHECK(!l.make(IdParts{8, 0, 0, 0}, id, &err) && err.find("priority") != std::string::npos);
  CHECK(!l.make(IdParts{0, 0, 1024, 0}, id, &err));
  CHECK(l.make(IdParts{7, 255, 1023, 255}, id, &err) && id == 0x1FFFFFFFu);
}

TEST(heartbeat_matches_c) {
  anv_heartbeat_t c{};
  c.state = ANV_NODE_STATE_ARMED;
  c.can_state = 2;
  c.debug_level = 3;
  c.leader_seen = true;
  c.type_id = 0xBEEF;
  c.schema_hash = 0x1234;
  c.uptime_s = 65535;
  uint8_t out[8];
  const uint8_t n = anv_heartbeat_pack(out, &c);
  Values v{{"state", one(int64_t{3})}, {"can_state", one(int64_t{2})}, {"debug_level", one(int64_t{3})},
           {"leader_seen", one(true)}, {"type_id", one(uint64_t{0xBEEF})}, {"schema_hash", one(int64_t{0x1234})},
           {"uptime_s", one(int64_t{65535})}};
  CHECK(same(enc(pmsg("heartbeat"), v), out, n));
  // and with labels, physical mode
  Values labels = v;
  labels["state"] = one(std::string("armed"));
  labels["can_state"] = one(std::string("bus_off"));
  CHECK(same(enc(pmsg("heartbeat"), labels, false), out, n));

  Values back;
  std::string err;
  CHECK(decode(pmsg("heartbeat"), out, n, back, &err));
  CHECK(std::get<std::string>(back["state"][0]) == "armed");
  CHECK(std::get<uint64_t>(back["type_id"][0]) == 0xBEEF);
  CHECK(std::get<bool>(back["leader_seen"][0]) == true);
  DecodeOptions raw;
  raw.raw = true;
  CHECK(decode(pmsg("heartbeat"), out, n, back, &err, raw));
  CHECK(std::get<uint64_t>(back["state"][0]) == 3);
}

TEST(service_and_arrays_match_c) {
  anv_service_t c{};
  c.op = ANV_SERVICE_OP_SET;
  c.status = 7;
  c.txn = 0x5A;
  c.index = 0xF001;
  c.value = 0xDEADBEEF;
  uint8_t out[8];
  uint8_t n = anv_service_pack(out, &c);
  CHECK(same(enc(pmsg("service"), {{"op", one(std::string("set"))}, {"status", one(int64_t{7})},
                                   {"txn", one(int64_t{0x5A})}, {"index", one(int64_t{0xF001})},
                                   {"value", one(uint64_t{0xDEADBEEF})}}),
             out, n));

  anv_id_request_t r{};
  const uint8_t mac[6] = {0x24, 0x6F, 0x28, 0xAA, 0xBB, 0xCC};
  std::memcpy(r.mac, mac, 6);
  r.type_id = 2;
  n = anv_id_request_pack(out, &r);
  FieldValue m;
  for (uint8_t b : mac) m.push_back(int64_t{b});
  CHECK(same(enc(pmsg("id_request"), {{"mac", m}, {"type_id", one(int64_t{2})}}), out, n));
  // an array of the wrong length is refused
  std::vector<uint8_t> bytes;
  std::string err;
  CHECK(!encode(pmsg("id_request"), {{"mac", FieldValue{int64_t{1}, int64_t{2}}}}, bytes, &err));
}

TEST(scaled_signed_matches_c) {
  const NodeType* mc = t::schema().type("motor_controller");
  CHECK(mc != nullptr);
  if (!mc) return;
  anv_motor_controller_setpoint_t c{};
  c.mode = 2;
  c.target[0] = anv_motor_controller_setpoint_target_from_phys(1.5f);
  c.target[1] = anv_motor_controller_setpoint_target_from_phys(-2.0f);
  c.accel_limit = 1000;
  uint8_t out[8];
  const uint8_t n = anv_motor_controller_setpoint_pack(out, &c);
  const Message& sp = *mc->message("setpoint");
  CHECK(same(enc(sp, {{"mode", one(std::string("velocity"))}, {"target", FieldValue{1.5, -2.0}},
                      {"accel_limit", one(10.0)}},
                 false),
             out, n));
  Values back;
  std::string err;
  CHECK(decode(sp, out, n, back, &err));
  CHECK(std::fabs(std::get<double>(back["target"][1]) + 2.0) < 1e-9);

  // physical limits and type range are enforced
  std::vector<uint8_t> bytes;
  CHECK(!encode(sp, {{"target", one(1e9)}}, bytes, &err));
  CHECK(!encode(sp, {{"mode", one(std::string("warp"))}}, bytes, &err));
  CHECK(!encode(sp, {{"nonsense", one(1.0)}}, bytes, &err) && err.find("nonsense") != std::string::npos);
}

TEST(const_fields) {
  const Message& e = pmsg("estop");
  std::vector<uint8_t> bytes = enc(e, {{"reason", one(int64_t{4})}}, false);
  CHECK(bytes.size() == 2 && bytes[0] == 0xE5 && bytes[1] == 4);
  Values v;
  std::string err;
  const uint8_t bad[2] = {0x00, 0x04};
  CHECK(!decode(e, bad, 2, v, &err));
  DecodeOptions lax;
  lax.strict = false;
  CHECK(decode(e, bad, 2, v, &err, lax));
  CHECK(!decode(e, bytes.data(), 1, v, &err));  // short frame
}

TEST(decode_frame_by_port) {
  const Schema& s = t::schema();
  const NodeType* mc = s.type("motor_controller");
  Frame f;
  std::string err;
  CHECK(encode_frame(s, *mc->message("setpoint"), {{"target", FieldValue{1.0, 2.0}}}, 1, 20, f, &err));
  CHECK(f.extended && f.len == 7);
  DecodedFrame d = decode_frame(s, f, mc);
  CHECK(d.message != nullptr && d.message->name == "setpoint" && d.error.empty());
  CHECK(d.id.src == 1 && d.id.dst == 20 && d.id.port == mc->message("setpoint")->port);
  d = decode_frame(s, f, nullptr);  // node-type port, sender unknown
  CHECK(d.message == nullptr);
  CHECK(encode_frame(s, pmsg("sync"), {{"counter", one(int64_t{9})}}, 1, 0, f, &err));
  d = decode_frame(s, f, nullptr);  // protocol ports need no type
  CHECK(d.message != nullptr && d.message->name == "sync" && std::get<uint64_t>(d.values["counter"][0]) == 9);
  CHECK(!encode_frame(s, pmsg("sync"), {}, 300, 0, f, &err));  // src doesn't fit
}

TEST(float_fields_round_trip) {
  // Find any f32 field in the schema and check bit-exact round trips.
  const Schema& s = t::schema();
  for (const NodeType* nt : [&] {
         std::vector<const NodeType*> v{&s.protocol};
         for (const NodeType& x : s.types) v.push_back(&x);
         return v;
       }()) {
    for (const Message& m : nt->messages) {
      for (const Field& f : m.fields) {
        if (!f.type.is_float || f.const_value) continue;
        std::vector<uint8_t> b;
        std::string err;
        EncodeOptions raw;
        raw.raw = true;
        CHECK(encode(m, {{f.name, FieldValue(f.count, Scalar{-123.25})}}, b, &err, raw));
        Values back;
        CHECK(decode(m, b.data(), b.size(), back, &err));
        CHECK(std::get<double>(back[f.name][0]) == -123.25);
      }
    }
  }
}

TEST_MAIN()
