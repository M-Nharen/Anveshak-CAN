// Linux SocketCAN implementation of ICanBus: raw socket, kernel receive timestamps, kernel-level
// ID filters, error frames enabled.
#pragma once

#include <array>
#include <atomic>
#include <cstdint>
#include <memory>
#include <string>

#include "anv_gateway/can_bus.hpp"

namespace anv::gw {

struct IdFilter {
  uint16_t id;
  uint16_t mask;
  bool accepts(uint16_t frame_id) const { return (frame_id & mask) == (id & mask); }
};

// The kernel receive filters the gateway installs: network, feedback, telemetry, sensor and heartbeat
// classes, plus node 31's service slot (0x5F8-0x5FF: NODE_ID_REQUEST from unconfigured boards and
// other hosts' NODE_ID_ASSIGN), plus text from nodes (class 7 sub 5). Commands (class 1), other
// service (5) and bootloader (7) frames never wake it. Defined here so tests can check every frame
// the gateway acts on gets through.
constexpr std::array<IdFilter, 7> kGatewayRxFilters = {{
    {0x000, 0x700}, {0x200, 0x700}, {0x300, 0x700}, {0x400, 0x700}, {0x600, 0x700}, {0x5F8, 0x7F8}, {0x705, 0x707},
}};

class SocketCanBus : public ICanBus {
 public:
  // Returns nullptr and fills *error if the interface cannot be opened.
  static std::unique_ptr<SocketCanBus> open(const std::string& interface_name, std::string* error);
  ~SocketCanBus() override;

  bool send(const CanFrame& frame) override;
  RecvStatus recv(CanFrame& out, int timeout_ms) override;
  void close() override;

 private:
  explicit SocketCanBus(int fd) : fd_(fd) {}
  int fd_;
  std::atomic<bool> closed_{false};
};

}  // namespace anv::gw
