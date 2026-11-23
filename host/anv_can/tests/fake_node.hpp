// A fake node for the host tests, written against the schema only (any node type works):
// heartbeat, ID request, the parameter service, stream rates, NMT from the leader, E-stop,
// leader tracking, and the command watchdog. It is what the tests need, not the real node
// library (that is firmware/lib's anv_node).
#pragma once

#include <atomic>
#include <chrono>
#include <cmath>
#include <map>
#include <mutex>
#include <thread>

#include "anv/codec.hpp"
#include "anv/host/allocator.hpp"
#include "anv/host/host.hpp"
#include "anv/test_bus.hpp"

namespace t {

class FakeNode {
 public:
  // id 0: no ID yet, ask the allocator (from the unassigned ID) with this MAC.
  FakeNode(anv::TestBus& bus, const anv::NodeType& type, uint8_t id, uint64_t mac = 0)
      : id_(id), s_(schema()), type_(type), link_(bus.connect()), mac_(mac) {
    for (const anv::Param* p : all_params()) {
      uint32_t w = 0;
      anv::host::value_to_wire(p->type, p->def.value_or(0), w, nullptr);
      params_[p->index] = w;
    }
    for (const anv::Message& m : type_.messages) {
      if (m.kind == "stream" && m.rate) rates_mhz_[m.port] = static_cast<uint32_t>(m.rate->def * 1000);
    }
  }
  ~FakeNode() { stop(); }

  void start() {
    running_ = true;
    th_ = std::thread([this] { run(); });
  }
  void stop() {
    if (!running_.exchange(false)) return;
    th_.join();
  }

  // What the tests look at.
  std::atomic<uint8_t> id_;
  std::atomic<bool> armed{false}, estop{false}, bad_hash{false};
  std::atomic<int> commands{0}, actions{0}, saves{0}, telemetry_sent{0};
  std::atomic<uint32_t> last_action_arg{0};
  std::atomic<uint8_t> leader{0};
  std::atomic<int> debug_sets{0};
  std::atomic<uint8_t> debug_level{0};
  uint32_t param(uint16_t index) {
    std::lock_guard<std::mutex> lk(mu_);
    return params_[index];
  }
  uint32_t rate_mhz(uint16_t port) {
    std::lock_guard<std::mutex> lk(mu_);
    return rates_mhz_[port];
  }

 private:
  using Clock = std::chrono::steady_clock;
  std::vector<const anv::Param*> all_params() const {
    std::vector<const anv::Param*> v;
    for (const auto& p : type_.params) v.push_back(&p);
    for (const auto& p : s_.protocol.params) v.push_back(&p);
    return v;
  }
  const anv::Param* find_param(uint16_t index) const {
    const anv::Param* p = type_.param(index);
    return p ? p : s_.protocol.param(index);
  }
  int64_t label(const char* en, const char* l) const {
    for (const auto& e : s_.protocol.enums) {
      if (e.first != en) continue;
      for (const auto& kv : e.second) {
        if (kv.first == l) return kv.second;
      }
    }
    return -1;
  }
  uint8_t src() const { return id_ ? id_.load() : static_cast<uint8_t>(s_.node_ids.unassigned); }

  void send(const anv::Message& m, const anv::Values& v, uint32_t dst, bool raw = false) {
    anv::Frame f;
    anv::EncodeOptions o;
    o.raw = raw;
    std::string err;
    if (anv::encode_frame(s_, m, v, src(), dst, f, &err, o)) link_->send(f);
  }
  static anv::Scalar n(uint64_t x) { return anv::Scalar{x}; }
  static int64_t get(const anv::Values& v, const char* k, size_t i = 0) {
    return static_cast<int64_t>(anv::as_double(v.at(k).at(i)));
  }

  void disarm() { armed = false; }

  void run() {
    const anv::NodeType& p = s_.protocol;
    auto next_hb = Clock::now();
    std::map<uint16_t, Clock::time_point> next_stream;
    Clock::time_point last_cmd{};
    while (running_) {
      anv::Frame f;
      if (link_->recv(f, 1) == anv::RecvStatus::Ok) handle(f, last_cmd);
      const auto now = Clock::now();
      if (now >= next_hb) {
        next_hb = now + std::chrono::milliseconds(id_ ? 50 : 30);
        if (id_ == 0) {
          anv::Values v;
          uint8_t mac[6];
          anv::host::IdAllocator::mac_to_bytes(mac_, mac);
          anv::FieldValue fv;
          for (uint8_t b : mac) fv.push_back(n(b));
          v["mac"] = fv;
          v["type_id"] = anv::one(n(type_.type_id));
          send(*p.message("id_request"), v, s_.node_ids.broadcast, true);
        } else {
          anv::Values v;
          v["state"] = anv::one(std::string(estop ? "estop" : armed ? "armed" : "idle"));
          v["type_id"] = anv::one(n(type_.type_id));
          v["schema_hash"] = anv::one(n(type_.schema_hash16() ^ (bad_hash ? 1u : 0u)));
          v["uptime_s"] = anv::one(n(5));
          v["leader_seen"] = anv::one(anv::Scalar{leader != 0});
          send(*p.message("heartbeat"), v, s_.node_ids.broadcast);
        }
      }
      // Command watchdog: outputs off when commands stop.
      if (armed) {
        for (const anv::Message& m : type_.messages) {
          if (m.kind == "command" && m.watchdog_ms && now - last_cmd > std::chrono::milliseconds(m.watchdog_ms)) {
            disarm();
          }
        }
      }
      if (id_ == 0) continue;
      for (const anv::Message& m : type_.messages) {
        if (m.kind != "stream") continue;
        const uint32_t mhz = rate_mhz(m.port);
        auto& due = next_stream[m.port];
        if (mhz == 0 || now < due) continue;
        due = now + std::chrono::microseconds(static_cast<int64_t>(1e9 / mhz));
        send(m, {}, s_.node_ids.broadcast);  // all-zero values are fine for the tests
        ++telemetry_sent;
      }
    }
  }

  void handle(const anv::Frame& f, Clock::time_point& last_cmd) {
    const anv::IdParts idp = s_.id_layout.parse(f.id);
    const anv::NodeType& p = s_.protocol;
    const anv::Message* m = idp.port <= s_.protocol_last_port ? p.message(static_cast<uint16_t>(idp.port))
                                                               : type_.message(static_cast<uint16_t>(idp.port));
    if (!m || m->kind == "frame") return;
    anv::Values v;
    std::string err;
    anv::DecodeOptions o;
    o.raw = true;
    o.labels = false;
    if (!anv::decode(*m, f.data.data(), f.len, v, &err, o)) return;
    const bool to_me = idp.dst == s_.node_ids.broadcast || (id_ != 0 && idp.dst == id_);

    if (m->name == "host_heartbeat") {
      if (get(v, "role") != label("host_role", "leader")) return;
      const auto term = static_cast<uint16_t>(get(v, "term"));
      if (leader != idp.src || term != term_) disarm();  // a new leader or term: disarm
      leader = static_cast<uint8_t>(idp.src);
      term_ = term;
    } else if (m->name == "estop") {
      estop = true;
      disarm();
    } else if (m->name == "estop_release" && idp.src == leader) {
      estop = false;
    } else if (m->name == "nmt" && to_me && idp.src == leader && get(v, "term") == term_) {
      const int64_t c = get(v, "command");
      if (c == label("nmt_command", "arm") && !estop) {
        armed = true;
        last_cmd = Clock::now();  // the watchdog runs from the moment of arming
      }
      if (c == label("nmt_command", "disarm")) disarm();
    } else if (m->name == "debug_level" && to_me && idp.src == leader) {
      debug_level = static_cast<uint8_t>(get(v, "level"));
      ++debug_sets;
    } else if (m->name == "id_assign" && id_ == 0 && idp.dst == s_.node_ids.unassigned) {
      uint8_t mac[6];
      for (size_t i = 0; i < 6; ++i) mac[i] = static_cast<uint8_t>(get(v, "mac", i));
      if (anv::host::IdAllocator::mac_from_bytes(mac) == mac_) id_ = static_cast<uint8_t>(get(v, "node_id"));
    } else if (m->name == "service" && id_ != 0 && idp.dst == id_) {
      service(static_cast<uint8_t>(idp.src), v);
    } else if (m->kind == "command" && idp.dst == id_ && idp.src == leader) {
      ++commands;
      last_cmd = Clock::now();
    }
  }

  void service(uint8_t from, anv::Values v) {
    const int64_t op = get(v, "op");
    const auto index = static_cast<uint16_t>(get(v, "index"));
    auto value = static_cast<uint32_t>(get(v, "value"));
    const char* status = "ok";
    {
      std::lock_guard<std::mutex> lk(mu_);
      if (op == label("service_op", "get")) {
        if (!find_param(index)) status = "bad_index";
        else value = params_[index];
      } else if (op == label("service_op", "set")) {
        const anv::Param* prm = find_param(index);
        if (!prm) status = "bad_index";
        else if (prm->access == "ro") status = "read_only";
        else if (prm->disarmed_only && armed) status = "not_while_armed";
        else params_[index] = value;
      } else if (op == label("service_op", "save")) {
        ++saves;
      } else if (op == label("service_op", "get_rate")) {
        value = rates_mhz_[index];
      } else if (op == label("service_op", "set_rate")) {
        rates_mhz_[index] = value;
      } else if (op == label("service_op", "action")) {
        ++actions;
        last_action_arg = value;
      } else {
        status = "bad_op";
      }
    }
    v["status"] = anv::one(n(static_cast<uint64_t>(label("service_status", status))));
    v["value"] = anv::one(n(value));
    send(*s_.protocol.message("service"), v, from, true);
  }

  const anv::Schema& s_;
  const anv::NodeType& type_;
  std::unique_ptr<anv::Link> link_;
  uint64_t mac_;
  uint16_t term_ = 0;
  std::mutex mu_;
  std::map<uint16_t, uint32_t> params_;
  std::map<uint16_t, uint32_t> rates_mhz_;
  std::atomic<bool> running_{false};
  std::thread th_;
};

}  // namespace t
