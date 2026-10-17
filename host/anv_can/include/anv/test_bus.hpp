// In-process CAN bus for tests: a frame sent on one port reaches every other port whose filters
// accept it. No arbitration, no bit timing, no echo of a port's own frames (SocketCAN's default).
#pragma once

#include <functional>
#include <memory>

#include "anv/link.hpp"

namespace anv {

struct TestBusShared;

class TestBus {
 public:
  // Ports queue at most this many frames; more are dropped and counted (like a kernel socket).
  explicit TestBus(size_t rx_queue = 4096);
  ~TestBus();
  TestBus(const TestBus&) = delete;
  TestBus& operator=(const TestBus&) = delete;

  // A new port. It stays valid after the bus is destroyed (it just goes silent).
  std::unique_ptr<Link> connect();

  // Called for every frame put on the bus; return false to "lose" it (fault-injection in tests).
  void set_tap(std::function<bool(const Frame&)> tap);

  uint64_t frames_sent() const;

 private:
  std::shared_ptr<TestBusShared> shared_;
};

}  // namespace anv
