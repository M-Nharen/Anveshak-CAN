#include "anv/socketcan_link.hpp"

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

namespace anv {

namespace {

constexpr int64_t kNsPerSec = 1000000000;
constexpr int64_t kNsPerMs = 1000000;

int64_t mono_ns() {
  timespec ts{};
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return static_cast<int64_t>(ts.tv_sec) * kNsPerSec + ts.tv_nsec;
}

int64_t realtime_ns() {
  timespec ts{};
  clock_gettime(CLOCK_REALTIME, &ts);
  return static_cast<int64_t>(ts.tv_sec) * kNsPerSec + ts.tv_nsec;
}

std::string errno_text(const std::string& what) { return what + ": " + std::strerror(errno); }

bool install_filters(int fd, const std::vector<Filter>& filters) {
  if (filters.empty()) {
    can_filter all{0, 0};
    return setsockopt(fd, SOL_CAN_RAW, CAN_RAW_FILTER, &all, sizeof all) == 0;
  }
  std::vector<can_filter> k(filters.size());
  for (size_t i = 0; i < filters.size(); ++i) {
    const Filter& f = filters[i];
    // EFF_FLAG in both id and mask makes the kernel check the frame kind too; RTR frames never match.
    k[i].can_id = (f.id & (f.extended ? CAN_EFF_MASK : CAN_SFF_MASK)) | (f.extended ? CAN_EFF_FLAG : 0);
    k[i].can_mask = (f.mask & (f.extended ? CAN_EFF_MASK : CAN_SFF_MASK)) | CAN_EFF_FLAG | CAN_RTR_FLAG;
  }
  return setsockopt(fd, SOL_CAN_RAW, CAN_RAW_FILTER, k.data(),
                    static_cast<socklen_t>(k.size() * sizeof(can_filter))) == 0;
}

}  // namespace

std::unique_ptr<SocketCanLink> SocketCanLink::open(const std::string& interface_name,
                                                   const SocketCanOptions& options, std::string* error) {
  const int fd = ::socket(PF_CAN, SOCK_RAW | SOCK_CLOEXEC, CAN_RAW);
  if (fd < 0) {
    if (error) *error = errno_text("socket(PF_CAN)");
    return nullptr;
  }
  auto fail = [&](const std::string& what) {
    if (error) *error = errno_text(what);
    ::close(fd);
    return std::unique_ptr<SocketCanLink>();
  };

  if (!install_filters(fd, options.filters)) return fail("CAN_RAW_FILTER");
  can_err_mask_t err_mask = options.error_frames ? (CAN_ERR_BUSOFF | CAN_ERR_RESTARTED | CAN_ERR_CRTL) : 0;
  if (setsockopt(fd, SOL_CAN_RAW, CAN_RAW_ERR_FILTER, &err_mask, sizeof err_mask) < 0) {
    return fail("CAN_RAW_ERR_FILTER");
  }
  const int own = options.receive_own ? 1 : 0;
  if (setsockopt(fd, SOL_CAN_RAW, CAN_RAW_RECV_OWN_MSGS, &own, sizeof own) < 0) {
    return fail("CAN_RAW_RECV_OWN_MSGS");
  }
  const int on = 1;
  if (setsockopt(fd, SOL_SOCKET, SO_TIMESTAMPNS, &on, sizeof on) < 0) return fail("SO_TIMESTAMPNS");
  setsockopt(fd, SOL_SOCKET, SO_RXQ_OVFL, &on, sizeof on);  // best effort: kernel drop counter
  const int rcvbuf = 1 << 20;
  setsockopt(fd, SOL_SOCKET, SO_RCVBUF, &rcvbuf, sizeof rcvbuf);  // best effort

  ifreq ifr{};
  std::strncpy(ifr.ifr_name, interface_name.c_str(), IFNAMSIZ - 1);
  if (ioctl(fd, SIOCGIFINDEX, &ifr) < 0) return fail("interface " + interface_name);

  sockaddr_can addr{};
  addr.can_family = AF_CAN;
  addr.can_ifindex = ifr.ifr_ifindex;
  if (bind(fd, reinterpret_cast<sockaddr*>(&addr), sizeof addr) < 0) return fail("bind");

  return std::unique_ptr<SocketCanLink>(new SocketCanLink(fd));
}

SocketCanLink::~SocketCanLink() {
  if (fd_ >= 0) ::close(fd_);
}

void SocketCanLink::close() { closed_.store(true); }

bool SocketCanLink::set_filters(const std::vector<Filter>& filters) {
  return !closed_.load() && install_filters(fd_, filters);
}

LinkStats SocketCanLink::stats() const {
  std::lock_guard<std::mutex> lk(stats_mu_);
  return stats_;
}

bool SocketCanLink::send(const Frame& frame) {
  can_frame cf{};
  cf.can_id = frame.extended ? ((frame.id & CAN_EFF_MASK) | CAN_EFF_FLAG) : (frame.id & CAN_SFF_MASK);
  cf.can_dlc = frame.len > 8 ? 8 : frame.len;
  std::memcpy(cf.data, frame.data.data(), cf.can_dlc);
  // MSG_DONTWAIT: if the TX queue is full, fail now rather than stall the control cycle.
  const bool ok = !closed_.load() &&
                  ::send(fd_, &cf, sizeof cf, MSG_DONTWAIT) == static_cast<ssize_t>(sizeof cf);
  std::lock_guard<std::mutex> lk(stats_mu_);
  ++(ok ? stats_.tx_frames : stats_.tx_dropped);
  return ok;
}

RecvStatus SocketCanLink::recv(Frame& out, int timeout_ms) {
  const int64_t deadline = mono_ns() + static_cast<int64_t>(timeout_ms < 0 ? 0 : timeout_ms) * kNsPerMs;
  for (;;) {
    if (closed_.load()) return RecvStatus::Closed;
    const int64_t left_ms = (deadline - mono_ns()) / kNsPerMs;
    pollfd pfd{fd_, POLLIN, 0};
    // Short slices so close() takes effect quickly even with a long timeout.
    const int slice = static_cast<int>(left_ms < 0 ? 0 : (left_ms > 20 ? 20 : left_ms));
    const int pr = ::poll(&pfd, 1, slice);
    if (pr < 0 && errno != EINTR) return RecvStatus::Closed;
    if (pr > 0) {
      can_frame cf{};
      iovec iov{&cf, sizeof cf};
      char ctrl[CMSG_SPACE(sizeof(timespec)) + CMSG_SPACE(sizeof(uint32_t))];
      msghdr msg{};
      msg.msg_iov = &iov;
      msg.msg_iovlen = 1;
      msg.msg_control = ctrl;
      msg.msg_controllen = sizeof ctrl;
      const ssize_t n = ::recvmsg(fd_, &msg, MSG_DONTWAIT);
      if (n < 0 && errno != EAGAIN && errno != EINTR) return RecvStatus::Closed;  // interface gone
      if (n == static_cast<ssize_t>(sizeof cf)) {
        const int64_t mono_now = mono_ns();
        int64_t rx = mono_now;
        std::lock_guard<std::mutex> lk(stats_mu_);
        for (cmsghdr* c = CMSG_FIRSTHDR(&msg); c != nullptr; c = CMSG_NXTHDR(&msg, c)) {
          if (c->cmsg_level != SOL_SOCKET) continue;
          if (c->cmsg_type == SO_TIMESTAMPNS) {
            timespec ts;
            std::memcpy(&ts, CMSG_DATA(c), sizeof ts);
            // The kernel stamps with the realtime clock; turn that into an age on the monotonic clock.
            const int64_t age = realtime_ns() - (static_cast<int64_t>(ts.tv_sec) * kNsPerSec + ts.tv_nsec);
            if (age >= 0 && age < kNsPerSec) rx = mono_now - age;
          } else if (c->cmsg_type == SO_RXQ_OVFL) {
            uint32_t drops;
            std::memcpy(&drops, CMSG_DATA(c), sizeof drops);
            stats_.rx_dropped += drops - last_kernel_drops_;  // the kernel's counter is cumulative
            last_kernel_drops_ = drops;
          }
        }
        if (cf.can_id & CAN_ERR_FLAG) {
          ++stats_.error_frames;
          if (cf.can_id & CAN_ERR_BUSOFF) ++stats_.bus_off;
          if (cf.can_id & CAN_ERR_RESTARTED) ++stats_.restarts;
          continue;
        }
        if (cf.can_id & CAN_RTR_FLAG) continue;
        out = Frame{};
        out.rx_ns = rx;
        out.extended = (cf.can_id & CAN_EFF_FLAG) != 0;
        out.id = cf.can_id & (out.extended ? CAN_EFF_MASK : CAN_SFF_MASK);
        out.len = cf.can_dlc > 8 ? 8 : cf.can_dlc;
        std::memcpy(out.data.data(), cf.data, out.len);
        ++stats_.rx_frames;
        return RecvStatus::Ok;
      }
    }
    if (mono_ns() >= deadline) return RecvStatus::Timeout;
  }
}

}  // namespace anv
