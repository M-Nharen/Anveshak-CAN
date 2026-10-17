// Linux SocketCAN Link (real CAN interfaces and vcan): raw socket, kernel receive timestamps,
// kernel-level filters, error frames for bus-off counting.
//
// The bitrate is a property of the interface, not of the socket: set it when bringing the
// interface up (`ip link set can0 type can bitrate 500000 restart-ms 100`; see
// host/tools/rpi-bench/). restart-ms is what recovers from bus-off on Linux.
#pragma once

#include <atomic>
#include <memory>
#include <mutex>
#include <string>

#include "anv/link.hpp"

namespace anv {

struct SocketCanOptions {
  std::vector<Filter> filters;  // empty = everything
  bool error_frames = true;     // count bus-off / restarts (they are not returned by recv())
  bool receive_own = false;     // also see our own frames (for a sniffer next to a sender)
};

class SocketCanLink : public Link {
 public:
  // nullptr and *error filled if the interface cannot be opened.
  static std::unique_ptr<SocketCanLink> open(const std::string& interface_name,
                                             const SocketCanOptions& options, std::string* error);
  ~SocketCanLink() override;

  bool send(const Frame& frame) override;
  RecvStatus recv(Frame& out, int timeout_ms) override;
  bool set_filters(const std::vector<Filter>& filters) override;
  LinkStats stats() const override;
  void close() override;

 private:
  explicit SocketCanLink(int fd) : fd_(fd) {}
  int fd_;
  std::atomic<bool> closed_{false};
  mutable std::mutex stats_mu_;
  LinkStats stats_;
  uint32_t last_kernel_drops_ = 0;
};

}  // namespace anv
