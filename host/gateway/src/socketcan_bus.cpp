#include "anv_gateway/socketcan_bus.hpp"

#include <cerrno>
#include <cstring>

#include <linux/can.h>
#include <linux/can/error.h>
#include <linux/can/raw.h>
#include <net/if.h>
#include <poll.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <unistd.h>

#include "anv_gateway/time.hpp"

namespace anv::gw {

namespace {

std::string errno_text(const char* what) { return std::string(what) + ": " + std::strerror(errno); }

int64_t realtime_ns() {
  timespec ts{};
  clock_gettime(CLOCK_REALTIME, &ts);
  return static_cast<int64_t>(ts.tv_sec) * kNsPerSec + ts.tv_nsec;
}

}  // namespace

std::unique_ptr<SocketCanBus> SocketCanBus::open(const std::string& interface_name, std::string* error) {
  const int fd = ::socket(PF_CAN, SOCK_RAW | SOCK_CLOEXEC, CAN_RAW);
  if (fd < 0) {
    if (error) *error = errno_text("socket(PF_CAN)");
    return nullptr;
  }
  auto fail = [&](const char* what) {
    if (error) *error = errno_text(what);
    ::close(fd);
    return std::unique_ptr<SocketCanBus>();
  };

  can_filter filters[kGatewayRxFilters.size()];
  for (size_t i = 0; i < kGatewayRxFilters.size(); ++i) {
    filters[i].can_id = kGatewayRxFilters[i].id;
    filters[i].can_mask = CAN_SFF_MASK & kGatewayRxFilters[i].mask;
  }
  if (setsockopt(fd, SOL_CAN_RAW, CAN_RAW_FILTER, filters, sizeof filters) < 0) return fail("CAN_RAW_FILTER");

  can_err_mask_t err_mask = CAN_ERR_MASK;  // bus-off, error-passive, restarts, controller problems
  if (setsockopt(fd, SOL_CAN_RAW, CAN_RAW_ERR_FILTER, &err_mask, sizeof err_mask) < 0) return fail("CAN_RAW_ERR_FILTER");

  const int on = 1;
  if (setsockopt(fd, SOL_SOCKET, SO_TIMESTAMPNS, &on, sizeof on) < 0) return fail("SO_TIMESTAMPNS");

  const int rcvbuf = 1 << 20;
  setsockopt(fd, SOL_SOCKET, SO_RCVBUF, &rcvbuf, sizeof rcvbuf);  // best effort

  ifreq ifr{};
  std::strncpy(ifr.ifr_name, interface_name.c_str(), IFNAMSIZ - 1);
  if (ioctl(fd, SIOCGIFINDEX, &ifr) < 0) return fail(("interface " + interface_name).c_str());

  sockaddr_can addr{};
  addr.can_family = AF_CAN;
  addr.can_ifindex = ifr.ifr_ifindex;
  if (bind(fd, reinterpret_cast<sockaddr*>(&addr), sizeof addr) < 0) return fail("bind");

  return std::unique_ptr<SocketCanBus>(new SocketCanBus(fd));
}

SocketCanBus::~SocketCanBus() {
  if (fd_ >= 0) ::close(fd_);
}

void SocketCanBus::close() { closed_.store(true); }

bool SocketCanBus::send(const CanFrame& frame) {
  if (closed_.load()) return false;
  can_frame cf{};
  cf.can_id = frame.id & CAN_SFF_MASK;
  cf.can_dlc = frame.dlc > 8 ? 8 : frame.dlc;
  std::memcpy(cf.data, frame.data.data(), cf.can_dlc);
  // MSG_DONTWAIT: if the TX queue is full, fail now rather than stall the 20 ms cycle.
  const ssize_t n = ::send(fd_, &cf, sizeof cf, MSG_DONTWAIT);
  return n == static_cast<ssize_t>(sizeof cf);
}

RecvStatus SocketCanBus::recv(CanFrame& out, int timeout_ms) {
  const int64_t deadline = mono_ns() + static_cast<int64_t>(timeout_ms) * kNsPerMs;
  for (;;) {
    if (closed_.load()) return RecvStatus::Closed;
    const int64_t left_ms = (deadline - mono_ns()) / kNsPerMs;
    pollfd pfd{fd_, POLLIN, 0};
    // Poll in short slices so close() takes effect quickly even with a long timeout.
    const int slice = static_cast<int>(left_ms < 0 ? 0 : (left_ms > 20 ? 20 : left_ms));
    const int pr = ::poll(&pfd, 1, slice);
    if (pr < 0 && errno != EINTR) return RecvStatus::Closed;
    if (pr > 0) {
      can_frame cf{};
      iovec iov{&cf, sizeof cf};
      char ctrl[CMSG_SPACE(sizeof(timespec))];
      msghdr msg{};
      msg.msg_iov = &iov;
      msg.msg_iovlen = 1;
      msg.msg_control = ctrl;
      msg.msg_controllen = sizeof ctrl;
      const ssize_t n = ::recvmsg(fd_, &msg, MSG_DONTWAIT);
      if (n == static_cast<ssize_t>(sizeof cf)) {
        const int64_t mono_now = mono_ns();
        int64_t rx = mono_now;
        for (cmsghdr* c = CMSG_FIRSTHDR(&msg); c != nullptr; c = CMSG_NXTHDR(&msg, c)) {
          if (c->cmsg_level == SOL_SOCKET && c->cmsg_type == SO_TIMESTAMPNS) {
            timespec ts;
            std::memcpy(&ts, CMSG_DATA(c), sizeof ts);
            // The kernel stamps with the realtime clock. Convert to an age and subtract it from
            // the monotonic clock, so the frame time is accurate to when it hit the interface.
            const int64_t age = realtime_ns() - (static_cast<int64_t>(ts.tv_sec) * kNsPerSec + ts.tv_nsec);
            if (age >= 0 && age < kNsPerSec) rx = mono_now - age;
          }
        }
        out = CanFrame{};
        out.rx_ns = rx;
        if (cf.can_id & CAN_ERR_FLAG) {
          out.is_error = true;
          out.err_class = cf.can_id & CAN_ERR_MASK;
          out.dlc = cf.can_dlc > 8 ? 8 : cf.can_dlc;
          std::memcpy(out.data.data(), cf.data, out.dlc);
          return RecvStatus::Ok;
        }
        if (!(cf.can_id & (CAN_EFF_FLAG | CAN_RTR_FLAG))) {  // classic CAN 2.0A data frames only
          out.id = static_cast<uint16_t>(cf.can_id & CAN_SFF_MASK);
          out.dlc = cf.can_dlc > 8 ? 8 : cf.can_dlc;
          std::memcpy(out.data.data(), cf.data, out.dlc);
          return RecvStatus::Ok;
        }
      }
    }
    if (mono_ns() >= deadline) return RecvStatus::Timeout;
  }
}

}  // namespace anv::gw
