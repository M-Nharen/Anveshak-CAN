// The gateway link: docs/architecture.md "Host software" > "The gateway link". A small UDP
// protocol between the two hosts' gateways, so drive targets, the debug level and dashboard
// control actions cross hosts without depending on ROS 2 (section 10's "Humble and Jazzy, on
// either host, at the same time"). Wire format is documented in defs/gateway_link.md.
//
// IPv4 only (the rover's host-to-host Ethernet has no need for anything else); peer_host is a
// dotted address, not a hostname. Threading: start() launches one RX thread; send_*() and tick()
// are called from the owning Gateway's control loop and are thread-safe against the RX thread.
#pragma once

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "anv_gateway/events.hpp"
#include "anv_gateway/messages.hpp"
#include "anv_gateway/protocol.hpp"

namespace anv::gw {

constexpr int64_t kLinkAlivePeriodNs = 100 * kNsPerMs;  // 10 Hz "I'm alive"
constexpr int64_t kLinkLostNs = 1 * kNsPerSec;          // docs: "GATEWAY_LINK_LOST after 1 s"

struct GatewayLinkConfig {
  uint16_t listen_port = 0;
  std::string peer_host;  // dotted IPv4, e.g. "10.0.0.2" or "127.0.0.1" for tests
  uint16_t peer_port = 0;
};

using LinkTargetsCallback = std::function<void(uint8_t from_host, const std::array<WheelTarget, kNumDrives>&)>;
using LinkDebugCallback = std::function<void(uint8_t from_host, const DebugLevelMsg&)>;

class GatewayLink {
 public:
  explicit GatewayLink(GatewayLinkConfig cfg);
  ~GatewayLink();
  GatewayLink(const GatewayLink&) = delete;
  GatewayLink& operator=(const GatewayLink&) = delete;

  bool start(std::string* error);
  void stop();

  // Called from the control loop at whatever cadence it likes; these are cheap and only actually
  // put a datagram on the wire, there is no internal timer (the caller decides "every 100 ms").
  void send_alive(uint8_t host_id, Role role, uint16_t term, uint16_t uptime_s);
  void send_targets(uint8_t host_id, const std::array<WheelTarget, kNumDrives>& targets);
  void send_debug(uint8_t host_id, const DebugLevelMsg& msg);

  // Call at least every ~100 ms. Raises GatewayLinkLost once the peer has been silent for 1 s,
  // and GatewayLinkUp when a datagram arrives for the first time or after a loss.
  void tick(int64_t now_ns, std::vector<GatewayEvent>& events);

  void set_on_targets(LinkTargetsCallback cb);
  void set_on_debug(LinkDebugCallback cb);

  // True if a datagram has arrived within the last kLinkLostNs.
  bool peer_up(int64_t now_ns) const;

 private:
  void rx_loop();
  bool send_datagram(const uint8_t* data, size_t len);

  GatewayLinkConfig cfg_;
  int fd_ = -1;
  uint32_t peer_addr_ = 0;  // network byte order; resolved once in start()
  std::atomic<bool> running_{false};
  std::thread thread_;

  std::atomic<uint16_t> tx_seq_{0};
  std::atomic<int64_t> last_rx_ns_{0};
  std::atomic<bool> was_up_{false};

  std::mutex cb_mu_;
  LinkTargetsCallback on_targets_;
  LinkDebugCallback on_debug_;
};

}  // namespace anv::gw
