// A tiny test harness (no external dependency, so CI needs nothing but a compiler and CMake).
#pragma once

#include <atomic>
#include <chrono>
#include <cstdio>
#include <functional>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "anv_gateway/decoder.hpp"
#include "anv_gateway/encoder.hpp"
#include "anv_gateway/fake_bus.hpp"
#include "anv_gateway/gateway.hpp"
#include "anv_gateway/time.hpp"

namespace t {

struct Case {
  const char* name;
  void (*fn)();
};
inline std::vector<Case>& cases() {
  static std::vector<Case> c;
  return c;
}
inline int& failures() {
  static int f = 0;
  return f;
}
struct Reg {
  Reg(const char* n, void (*f)()) { cases().push_back({n, f}); }
};

inline int run_all() {
  int bad = 0;
  for (const Case& c : cases()) {
    const int before = failures();
    std::printf("[ RUN  ] %s\n", c.name);
    std::fflush(stdout);
    c.fn();
    if (failures() != before) {
      std::printf("[ FAIL ] %s\n", c.name);
      ++bad;
    } else {
      std::printf("[  OK  ] %s\n", c.name);
    }
  }
  std::printf("%zu tests, %d failed\n", cases().size(), bad);
  return bad == 0 ? 0 : 1;
}

}  // namespace t

#define TEST(name)                          \
  static void name();                       \
  static t::Reg reg_##name(#name, name);    \
  static void name()

#define CHECK(cond)                                                                 \
  do {                                                                              \
    if (!(cond)) {                                                                  \
      std::fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, #cond); \
      ++t::failures();                                                              \
    }                                                                               \
  } while (0)

#define CHECK_EQ(a, b)                                                                                   \
  do {                                                                                                   \
    const auto va = (a);                                                                                 \
    const auto vb = (b);                                                                                 \
    if (!(va == vb)) {                                                                                   \
      std::fprintf(stderr, "%s:%d: CHECK_EQ failed: %s (%lld) != %s (%lld)\n", __FILE__, __LINE__, #a,   \
                   static_cast<long long>(va), #b, static_cast<long long>(vb));                          \
      ++t::failures();                                                                                   \
    }                                                                                                    \
  } while (0)

namespace t {

using namespace anv::gw;

inline void sleep_ms(int ms) { std::this_thread::sleep_for(std::chrono::milliseconds(ms)); }

// Polls pred every 2 ms; true if it became true within timeout_ms.
inline bool wait_until(const std::function<bool()>& pred, int timeout_ms) {
  const int64_t end = mono_ns() + static_cast<int64_t>(timeout_ms) * kNsPerMs;
  while (mono_ns() < end) {
    if (pred()) return true;
    sleep_ms(2);
  }
  return pred();
}

inline GatewayConfig host_config(uint8_t host_id, bool auto_takeover = false) {
  GatewayConfig c;
  c.host_id = host_id;
  c.tie_break = host_id;
  c.auto_takeover = auto_takeover;
  c.rt_priority = 0;  // tests must not need root
  return c;
}

// Passive tap: records everything on the bus, with the hub's timestamps.
class Recorder {
 public:
  explicit Recorder(FakeBusHub& hub) : port_(hub.connect()), thread_([this] { loop(); }) {}
  ~Recorder() { stop(); }
  void stop() {
    if (!running_.exchange(false)) return;
    port_->close();
    if (thread_.joinable()) thread_.join();
  }
  std::vector<Decoded> all() const {
    std::lock_guard<std::mutex> lk(mu_);
    return frames_;
  }
  // Frames of one message type, in arrival order.
  template <class M>
  std::vector<std::pair<int64_t, M>> of() const {
    std::vector<std::pair<int64_t, M>> out;
    std::lock_guard<std::mutex> lk(mu_);
    for (const Decoded& d : frames_)
      if (const M* m = std::get_if<M>(&d.msg)) out.emplace_back(d.rx_ns, *m);
    return out;
  }
  size_t count_id(uint16_t id) const {
    size_t n = 0;
    std::lock_guard<std::mutex> lk(mu_);
    for (const Decoded& d : frames_)
      if (d.id == id) ++n;
    return n;
  }
  // Raw frames with a given ID (for payload checks on frames the decoder also understands).
  std::vector<CanFrame> raw_with_id(uint16_t id) const {
    std::vector<CanFrame> out;
    std::lock_guard<std::mutex> lk(mu_);
    for (const CanFrame& f : raw_)
      if (f.id == id) out.push_back(f);
    return out;
  }

 private:
  void loop() {
    CanFrame f;
    while (running_.load()) {
      const RecvStatus st = port_->recv(f, 5);
      if (st == RecvStatus::Closed) break;
      if (st != RecvStatus::Ok) continue;
      Decoded d;
      std::lock_guard<std::mutex> lk(mu_);
      raw_.push_back(f);
      if (decode_frame(f, d)) frames_.push_back(d);
    }
  }
  std::unique_ptr<ICanBus> port_;
  std::atomic<bool> running_{true};
  mutable std::mutex mu_;
  std::vector<Decoded> frames_;
  std::vector<CanFrame> raw_;
  std::thread thread_;
};

// Pretends to be one node: sends NODE_HB at 10 Hz with an uptime the test controls.
class FakeNode {
 public:
  FakeNode(FakeBusHub& hub, uint8_t node_id) : node_id_(node_id), port_(hub.connect()), thread_([this] { loop(); }) {}
  ~FakeNode() {
    running_ = false;
    port_->close();
    if (thread_.joinable()) thread_.join();
  }
  void set_uptime(uint16_t s) { uptime_ = s; }
  void set_sending(bool on) { sending_ = on; }
  int64_t last_send_ns() const { return last_send_ns_.load(); }

 private:
  void loop() {
    int64_t next = mono_ns();
    while (running_.load()) {
      next += 100 * kNsPerMs;
      while (running_.load() && mono_ns() < next) sleep_ms(2);
      if (!sending_.load()) continue;
      NodeHbMsg hb;
      hb.node_id = node_id_;
      hb.state = static_cast<uint8_t>(NodeState::Disarmed);
      hb.node_type = node_id_ == kNodeFpga ? 4 : 1;
      hb.protocol_version = 1;
      hb.uptime_s = uptime_.load();
      last_send_ns_ = mono_ns();
      port_->send(make_node_hb(hb));
    }
  }
  uint8_t node_id_;
  std::unique_ptr<ICanBus> port_;
  std::atomic<bool> running_{true};
  std::atomic<bool> sending_{true};
  std::atomic<uint16_t> uptime_{0};
  std::atomic<int64_t> last_send_ns_{0};
  std::thread thread_;
};

}  // namespace t
