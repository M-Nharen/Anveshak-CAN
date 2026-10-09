// Linux SocketCAN implementation of ICanBus: raw socket, kernel receive timestamps, kernel-level
// ID filters, error frames enabled.
#pragma once

#include <atomic>
#include <memory>
#include <string>

#include "anv_gateway/can_bus.hpp"

namespace anv::gw {

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
