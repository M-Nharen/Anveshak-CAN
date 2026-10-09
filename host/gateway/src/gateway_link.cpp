#include "anv_gateway/gateway_link.hpp"

#include <arpa/inet.h>
#include <netinet/in.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>

#include <cerrno>
#include <cstring>

#include "anv_gateway/time.hpp"
#include "errors.h"

namespace anv::gw {

namespace {

// Wire format (see defs/gateway_link.md): a 6-byte header, then a type-specific payload.
// All multi-byte fields are little-endian; this is host-to-host between two copies of the same
// program, not a cross-language boundary, so a small hand-rolled codec (unlike CAN payloads,
// which must go through the generated DBC code) is the simplest thing that stays correct.
constexpr uint8_t kWireVersion = 1;
constexpr uint8_t kTypeAlive = 1;
constexpr uint8_t kTypeTargets = 2;
constexpr uint8_t kTypeDebug = 3;
constexpr size_t kHeaderLen = 6;

void put_u8(std::vector<uint8_t>& b, uint8_t v) { b.push_back(v); }
void put_u16(std::vector<uint8_t>& b, uint16_t v) {
  b.push_back(static_cast<uint8_t>(v & 0xFF));
  b.push_back(static_cast<uint8_t>((v >> 8) & 0xFF));
}
void put_i16(std::vector<uint8_t>& b, int16_t v) { put_u16(b, static_cast<uint16_t>(v)); }

uint16_t get_u16(const uint8_t* p) { return static_cast<uint16_t>(p[0]) | static_cast<uint16_t>(p[1] << 8); }
int16_t get_i16(const uint8_t* p) { return static_cast<int16_t>(get_u16(p)); }

int16_t scale_target(double v) {
  const double raw = v * kTargetScale;
  const double clamped = raw < -32768.0 ? -32768.0 : (raw > 32767.0 ? 32767.0 : raw);
  return static_cast<int16_t>(clamped);
}

uint16_t scale_accel(double v) {
  const double raw = v * kTargetScale;
  const double clamped = raw < 0.0 ? 0.0 : (raw > 65535.0 ? 65535.0 : raw);
  return static_cast<uint16_t>(clamped);
}

std::vector<uint8_t> header(uint8_t type, uint8_t host_id, uint16_t seq) {
  std::vector<uint8_t> b;
  put_u8(b, kWireVersion);
  put_u8(b, type);
  put_u8(b, host_id);
  put_u8(b, 0);  // reserved
  put_u16(b, seq);
  return b;
}

}  // namespace

GatewayLink::GatewayLink(GatewayLinkConfig cfg) : cfg_(std::move(cfg)) {}

GatewayLink::~GatewayLink() { stop(); }

bool GatewayLink::start(std::string* error) {
  fd_ = ::socket(AF_INET, SOCK_DGRAM | SOCK_CLOEXEC, 0);
  if (fd_ < 0) {
    if (error) *error = std::string("socket: ") + std::strerror(errno);
    return false;
  }
  const int on = 1;
  setsockopt(fd_, SOL_SOCKET, SO_REUSEADDR, &on, sizeof on);  // best effort

  sockaddr_in addr{};
  addr.sin_family = AF_INET;
  addr.sin_addr.s_addr = INADDR_ANY;
  addr.sin_port = htons(cfg_.listen_port);
  if (::bind(fd_, reinterpret_cast<sockaddr*>(&addr), sizeof addr) < 0) {
    if (error) *error = "bind port " + std::to_string(cfg_.listen_port) + ": " + std::strerror(errno);
    ::close(fd_);
    fd_ = -1;
    return false;
  }

  in_addr peer{};
  if (::inet_pton(AF_INET, cfg_.peer_host.c_str(), &peer) != 1) {
    if (error) *error = "bad peer address '" + cfg_.peer_host + "'";
    ::close(fd_);
    fd_ = -1;
    return false;
  }
  peer_addr_ = peer.s_addr;

  running_.store(true);
  thread_ = std::thread([this] { rx_loop(); });
  return true;
}

void GatewayLink::stop() {
  if (!running_.exchange(false)) return;
  if (fd_ >= 0) ::shutdown(fd_, SHUT_RDWR);
  if (thread_.joinable()) thread_.join();
  if (fd_ >= 0) ::close(fd_);
  fd_ = -1;
}

bool GatewayLink::send_datagram(const uint8_t* data, size_t len) {
  if (fd_ < 0) return false;
  sockaddr_in dest{};
  dest.sin_family = AF_INET;
  dest.sin_addr.s_addr = peer_addr_;
  dest.sin_port = htons(cfg_.peer_port);
  const ssize_t n = ::sendto(fd_, data, len, MSG_DONTWAIT | MSG_NOSIGNAL, reinterpret_cast<sockaddr*>(&dest),
                              sizeof dest);
  return n == static_cast<ssize_t>(len);
}

void GatewayLink::send_alive(uint8_t host_id, Role role, uint16_t term, uint16_t uptime_s) {
  std::vector<uint8_t> b = header(kTypeAlive, host_id, tx_seq_.fetch_add(1));
  put_u8(b, static_cast<uint8_t>(role));
  put_u16(b, term);
  put_u16(b, uptime_s);
  send_datagram(b.data(), b.size());
}

void GatewayLink::send_targets(uint8_t host_id, const std::array<WheelTarget, kNumDrives>& targets) {
  std::vector<uint8_t> b = header(kTypeTargets, host_id, tx_seq_.fetch_add(1));
  for (const WheelTarget& t : targets) {
    put_u8(b, static_cast<uint8_t>(t.mode));
    put_i16(b, scale_target(t.a));
    put_i16(b, scale_target(t.b));
    put_u16(b, scale_accel(t.accel));
  }
  send_datagram(b.data(), b.size());
}

void GatewayLink::send_debug(uint8_t host_id, const DebugLevelMsg& msg) {
  std::vector<uint8_t> b = header(kTypeDebug, host_id, tx_seq_.fetch_add(1));
  put_u8(b, msg.target_node);
  put_u8(b, msg.level);
  put_u8(b, msg.category_mask);
  put_u16(b, msg.auto_off_timer_min);
  send_datagram(b.data(), b.size());
}

void GatewayLink::set_on_targets(LinkTargetsCallback cb) {
  std::lock_guard<std::mutex> lk(cb_mu_);
  on_targets_ = std::move(cb);
}

void GatewayLink::set_on_debug(LinkDebugCallback cb) {
  std::lock_guard<std::mutex> lk(cb_mu_);
  on_debug_ = std::move(cb);
}

bool GatewayLink::peer_up(int64_t now_ns) const {
  const int64_t last = last_rx_ns_.load();
  return last != 0 && now_ns - last <= kLinkLostNs;
}

void GatewayLink::tick(int64_t now_ns, std::vector<GatewayEvent>& events) {
  const bool up = peer_up(now_ns);
  const bool was = was_up_.load();
  if (up == was) return;
  was_up_.store(up);
  GatewayEvent e;
  e.time_ns = now_ns;
  if (up) {
    e.type = EventType::GatewayLinkUp;
    e.cleared = true;
  } else {
    e.type = EventType::GatewayLinkLost;
    e.code = ROVER_ERR_GATEWAY_LINK_LOST;
    e.severity = static_cast<uint8_t>(ROVER_SEVERITY_WARNING);
  }
  events.push_back(e);
}

void GatewayLink::rx_loop() {
  uint8_t buf[256];
  while (running_.load()) {
    pollfd pfd{fd_, POLLIN, 0};
    const int pr = ::poll(&pfd, 1, 20);
    if (pr < 0 && errno != EINTR) break;
    if (!running_.load()) break;
    if (pr <= 0 || !(pfd.revents & POLLIN)) continue;
    const ssize_t n = ::recv(fd_, buf, sizeof buf, MSG_DONTWAIT);
    if (n < static_cast<ssize_t>(kHeaderLen)) continue;
    if (buf[0] != kWireVersion) continue;
    const uint8_t type = buf[1];
    const uint8_t from_host = buf[2];
    const uint8_t* payload = buf + kHeaderLen;
    const size_t payload_len = static_cast<size_t>(n) - kHeaderLen;

    if (type == kTypeTargets && payload_len >= kNumDrives * 7) {
      std::array<WheelTarget, kNumDrives> targets{};
      for (size_t i = 0; i < kNumDrives; ++i) {
        const uint8_t* w = payload + i * 7;
        targets[i].mode = static_cast<DriveMode>(w[0]);
        targets[i].a = get_i16(w + 1) / kTargetScale;
        targets[i].b = get_i16(w + 3) / kTargetScale;
        targets[i].accel = get_u16(w + 5) / kTargetScale;
      }
      LinkTargetsCallback cb;
      {
        std::lock_guard<std::mutex> lk(cb_mu_);
        cb = on_targets_;
      }
      if (cb) cb(from_host, targets);
    } else if (type == kTypeDebug && payload_len >= 5) {
      DebugLevelMsg m;
      m.target_node = payload[0];
      m.level = payload[1];
      m.category_mask = payload[2];
      m.auto_off_timer_min = get_u16(payload + 3);
      LinkDebugCallback cb;
      {
        std::lock_guard<std::mutex> lk(cb_mu_);
        cb = on_debug_;
      }
      if (cb) cb(from_host, m);
    }
    // kTypeAlive carries nothing the caller needs beyond liveness, which last_rx_ns_ already gives.

    last_rx_ns_.store(mono_ns());
  }
}

}  // namespace anv::gw
