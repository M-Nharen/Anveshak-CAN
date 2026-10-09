// In-process CAN bus for tests: every frame sent on one port is delivered to every other port.
// No arbitration, no bit timing. It also does not echo a port's own frames, matching SocketCAN's
// default (receive-own-messages off).
#pragma once

#include <memory>
#include <mutex>
#include <vector>

#include "anv_gateway/can_bus.hpp"

namespace anv::gw {

struct FakeBusShared;  // defined in fake_bus.cpp

class FakeBusHub {
 public:
  FakeBusHub();
  ~FakeBusHub();
  FakeBusHub(const FakeBusHub&) = delete;
  FakeBusHub& operator=(const FakeBusHub&) = delete;

  // Creates a new port on the bus. Ports stay valid after the hub is destroyed (they just go silent).
  std::unique_ptr<ICanBus> connect();

  uint64_t frames_sent() const;

 private:
  std::shared_ptr<FakeBusShared> shared_;
};

}  // namespace anv::gw
