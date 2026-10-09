// Gateway + SimulatedRover over the in-process bus: lead, arm, drive, hand over, E-stop.
#include <array>
#include <memory>

#include "anv_gateway/gateway.hpp"
#include "anv_ros2/kinematics.hpp"
#include "anv_ros2/sim_rover.hpp"
#include "test_support.hpp"

using namespace anv::gw;
using anv::ros2::SimulatedRover;

namespace {

std::unique_ptr<Gateway> make_gateway(SimulatedRover& rover, uint8_t host) {
  GatewayConfig cfg;
  cfg.host_id = host;
  cfg.rt_priority = 0;
  return std::make_unique<Gateway>(cfg, rover.connect());
}

int alive_count(const GatewayState& s) {
  int n = 0;
  for (const auto& node : s.nodes) n += node.alive ? 1 : 0;
  return n;
}

// Keep targets fresh (the gateway zeroes anything older than 100 ms) until pred() holds.
template <typename F>
bool drive_until(Gateway& g, double rad_s, F pred, int timeout_ms) {
  std::array<WheelTarget, kNumDrives> t{};
  for (auto& w : t) w.a = w.b = rad_s;
  auto end = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeout_ms);
  while (std::chrono::steady_clock::now() < end) {
    g.set_targets(t);
    if (pred(g.state())) return true;
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
  }
  return pred(g.state());
}

bool all_drives(const GatewayState& s, uint8_t state, bool nonzero_speed) {
  int n = 0;
  for (const auto& node : s.nodes) {
    if (node.node_id < 4 || node.node_id > 9 || !node.has_fb_a) continue;
    if (node.fb_a.state != state) return false;
    if (nonzero_speed && node.fb_a.speed_a == 0) return false;
    if (!nonzero_speed && node.fb_a.speed_a != 0) return false;
    ++n;
  }
  return n == 6;
}

}  // namespace

int main() {
  SimulatedRover rover;
  auto g1 = make_gateway(rover, 1);
  rover.start();
  CHECK(g1->start());

  CHECK(wait_for([&] { return alive_count(g1->state()) >= 7; }, 3000));  // 6 drives + FPGA

  // Nobody leads until asked; commands are refused.
  CHECK(g1->arm() == CmdResult::NotMaster);
  CHECK(g1->request_lead(1) == CmdResult::Ok);
  CHECK(wait_for([&] { return g1->state().role == Role::Master; }, 2000));
  CHECK(g1->arm() == CmdResult::Ok);

  // Armed and driven: every drive reports ARMED (2) and a non-zero speed.
  CHECK(drive_until(*g1, 2.0, [](const GatewayState& s) { return all_drives(s, 2, true); }, 3000));

  // Stop refreshing targets: the 100 ms hold expires and speeds return to zero.
  CHECK(wait_for([&] { return all_drives(g1->state(), 2, false); }, 2000));

  // E-stop latches; release returns the nodes to DISARMED.
  CHECK(g1->estop() == CmdResult::Ok);
  CHECK(wait_for([&] { return all_drives(g1->state(), 4, false); }, 2000));
  CHECK(g1->release_estop() == CmdResult::Ok);
  CHECK(wait_for([&] { return all_drives(g1->state(), 1, false); }, 2000));

  // Handover: a second host takes the lead and the armed nodes disarm.
  CHECK(g1->arm() == CmdResult::Ok);
  CHECK(wait_for([&] { return all_drives(g1->state(), 2, false); }, 2000));
  auto g2 = make_gateway(rover, 2);
  CHECK(g2->start());
  // Let the new host hear the current master first, so its claim uses term = highest seen + 1.
  CHECK(wait_for([&] { return g2->state().believed_master == 1; }, 2000));
  CHECK(g1->request_lead(2) == CmdResult::Ok);
  CHECK(wait_for([&] { return g2->state().role == Role::Master; }, 2000));
  CHECK(wait_for([&] { return all_drives(g2->state(), 1, false); }, 2000));
  CHECK(g2->state().term > 1);

  g2->stop();
  g1->stop();
  rover.stop();

  if (failures()) return 1;
  std::puts("test_sim_rover: ok");
  return 0;
}
