// An in-process rover for development and tests: six drive nodes and the FPGA on a fake CAN bus.
// It follows the node rules the gateway relies on (outputs off by default, ARM from the master,
// disarm on command timeout / E-stop / master or term change, commands applied at SYNC), so the
// whole ROS stack can run and be tested without hardware or even vcan.
// The Python simulator in host/sim is the reference for bus-level behaviour; this one exists
// because a ROS launch test needs the bus inside the node's own process.
#pragma once

#include <array>
#include <atomic>
#include <cstdint>
#include <memory>
#include <thread>

#include "anv_gateway/can_bus.hpp"
#include "anv_gateway/decoder.hpp"
#include "anv_gateway/fake_bus.hpp"

namespace anv::ros2 {

class SimulatedRover {
 public:
  SimulatedRover();
  ~SimulatedRover();
  SimulatedRover(const SimulatedRover&) = delete;
  SimulatedRover& operator=(const SimulatedRover&) = delete;

  // A port on the simulated bus. Hand it to a gateway; connect() can be called more than once
  // (two gateways = two hosts).
  std::unique_ptr<anv::gw::ICanBus> connect();

  void start();
  void stop();

 private:
  struct Drive {
    uint8_t state = 1;  // NodeState::Disarmed
    uint8_t master = 0;
    uint8_t term_low = 0;
    int16_t pending_a = 0, pending_b = 0;  // received, applied at the next SYNC
    int16_t speed_a = 0, speed_b = 0;      // what the "motors" do
    int64_t last_cmd_ns = 0;
    int64_t next_hb_ns = 0;
  };

  void loop();
  void on_frame(const anv::gw::Decoded& d, int64_t now);
  void tick(int64_t now);
  void disarm(Drive& d);

  anv::gw::FakeBusHub hub_;
  std::unique_ptr<anv::gw::ICanBus> port_;
  std::array<Drive, 6> drives_;
  int64_t fpga_next_hb_ns_ = 0;
  int64_t boot_ns_ = 0;
  uint8_t known_master_ = 0;
  uint8_t known_term_ = 0;  // low 8 bits, as carried in NMT
  std::atomic<bool> running_{false};
  std::thread thread_;
};

}  // namespace anv::ros2
