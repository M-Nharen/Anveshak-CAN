// The UDP gateway link, end to end on loopback (docs/architecture.md "The gateway link"):
// target forwarding and link-loss detection. No CAN bus involved.
#include "anv_gateway/gateway_link.hpp"

#include <atomic>
#include <cmath>
#include <mutex>

#include "test_util.hpp"

using namespace anv::gw;
using namespace t;

namespace {

GatewayLinkConfig cfg(uint16_t listen_port, uint16_t peer_port) {
  GatewayLinkConfig c;
  c.listen_port = listen_port;
  c.peer_host = "127.0.0.1";
  c.peer_port = peer_port;
  return c;
}

}  // namespace

TEST(targets_sent_on_one_end_arrive_on_the_other) {
  GatewayLink orin(cfg(23501, 23502));
  GatewayLink rpi(cfg(23502, 23501));

  std::mutex mu;
  uint8_t got_from = 0;
  std::array<WheelTarget, kNumDrives> got{};
  bool got_any = false;
  rpi.set_on_targets([&](uint8_t from_host, const std::array<WheelTarget, kNumDrives>& t) {
    std::lock_guard<std::mutex> lk(mu);
    got_from = from_host;
    got = t;
    got_any = true;
  });

  std::string err;
  CHECK(orin.start(&err));
  CHECK(rpi.start(&err));

  std::array<WheelTarget, kNumDrives> sent{};
  for (uint8_t i = 0; i < kNumDrives; ++i) {
    sent[i].mode = DriveMode::Velocity;
    sent[i].a = 1.0 + i * 0.1;
    sent[i].b = -1.0 - i * 0.1;
    sent[i].accel = 5.0 + i;
  }
  // UDP on loopback can occasionally drop under load; keep sending until it is seen.
  CHECK(wait_until(
      [&] {
        orin.send_targets(kNodeOrin, sent);
        std::lock_guard<std::mutex> lk(mu);
        return got_any;
      },
      2000));

  {
    std::lock_guard<std::mutex> lk(mu);
    CHECK_EQ(got_from, kNodeOrin);
    for (uint8_t i = 0; i < kNumDrives; ++i) {
      CHECK(std::abs(got[i].a - sent[i].a) < 0.02);
      CHECK(std::abs(got[i].b - sent[i].b) < 0.02);
      CHECK(std::abs(got[i].accel - sent[i].accel) < 0.02);
      CHECK(got[i].mode == sent[i].mode);
    }
  }
  // stop() joins the RX thread, which may still be calling the callback above (it locks `mu`
  // itself): never hold `mu` here, or a datagram arriving at just the wrong moment deadlocks.
  orin.stop();
  rpi.stop();
}

TEST(a_debug_level_request_arrives_on_the_other_end) {
  GatewayLink orin(cfg(23503, 23504));
  GatewayLink rpi(cfg(23504, 23503));

  std::mutex mu;
  bool got_any = false;
  DebugLevelMsg got;
  rpi.set_on_debug([&](uint8_t /*from_host*/, const DebugLevelMsg& m) {
    std::lock_guard<std::mutex> lk(mu);
    got = m;
    got_any = true;
  });

  std::string err;
  CHECK(orin.start(&err));
  CHECK(rpi.start(&err));

  DebugLevelMsg msg;
  msg.target_node = 5;
  msg.level = 2;
  msg.category_mask = 0x0F;
  msg.auto_off_timer_min = 30;
  CHECK(wait_until(
      [&] {
        orin.send_debug(kNodeOrin, msg);
        std::lock_guard<std::mutex> lk(mu);
        return got_any;
      },
      2000));

  {
    std::lock_guard<std::mutex> lk(mu);
    CHECK_EQ(got.target_node, 5u);
    CHECK_EQ(got.level, 2u);
    CHECK_EQ(got.auto_off_timer_min, 30u);
  }
  orin.stop();
  rpi.stop();
}

TEST(the_link_is_marked_up_once_a_datagram_arrives_and_lost_after_1_s_of_silence) {
  GatewayLink orin(cfg(23505, 23506));
  GatewayLink rpi(cfg(23506, 23505));
  std::string err;
  CHECK(orin.start(&err));
  CHECK(rpi.start(&err));

  std::vector<GatewayEvent> events;
  // Nothing heard yet: no event, and tick() must not claim the link is up.
  rpi.tick(mono_ns(), events);
  CHECK(events.empty());
  CHECK(!rpi.peer_up(mono_ns()));

  orin.send_alive(kNodeOrin, Role::Standby, 0, 1);
  CHECK(wait_until([&] { return rpi.peer_up(mono_ns()); }, 500));
  events.clear();
  rpi.tick(mono_ns(), events);
  CHECK_EQ(events.size(), 1u);
  CHECK(events[0].type == EventType::GatewayLinkUp);

  orin.stop();  // the peer goes quiet
  CHECK(wait_until(
      [&] {
        std::vector<GatewayEvent> ev;
        rpi.tick(mono_ns(), ev);
        for (const auto& e : ev)
          if (e.type == EventType::GatewayLinkLost) return true;
        return false;
      },
      2000));
  rpi.stop();
}

int main() { return t::run_all(); }
