// anv_link: the host's CAN driver interface. Same idea as the firmware's anv_link: send,
// receive, filters, counters. Everything above it (anv_proto, tools, the host library) only
// talks to this, so tests run on the in-process TestBus and the rover runs on SocketCAN.
#pragma once

#include <array>
#include <cstdint>
#include <string>
#include <vector>

namespace anv {

constexpr uint32_t kExtIdMask = 0x1FFFFFFFu;
constexpr uint32_t kStdIdMask = 0x7FFu;

struct Frame {
  uint32_t id = 0;        // 29-bit when extended, else 11-bit
  bool extended = true;   // the v2 protocol only uses extended frames
  uint8_t len = 0;        // 0..8
  std::array<uint8_t, 8> data{};
  int64_t rx_ns = 0;      // CLOCK_MONOTONIC time the frame reached the host (0 for frames we built)
  bool is_error = false;  // a controller error report; err_class/data describe it
  uint32_t err_class = 0; // SocketCAN CAN_ERR_* class bits

  static Frame make(uint32_t id, const uint8_t* bytes, uint8_t n, bool extended = true);
};

// Accepts a frame when (frame.id & mask) == (id & mask) and the ID kind matches.
struct Filter {
  uint32_t id = 0;
  uint32_t mask = 0;
  bool extended = true;
  bool accepts(const Frame& f) const { return f.extended == extended && ((f.id ^ id) & mask) == 0; }
};

struct LinkStats {
  uint64_t tx_frames = 0;
  uint64_t rx_frames = 0;
  uint64_t tx_dropped = 0;   // send() refused: queue full or link down
  uint64_t rx_dropped = 0;   // frames the kernel (or test bus) dropped because we were slow
  uint64_t error_frames = 0;
  uint64_t bus_off = 0;      // times the controller went bus-off
  uint64_t restarts = 0;     // times it came back (SocketCAN restart-ms, or `ip link set up`)
};

// SocketCAN error-frame class bits (linux/can/error.h; copied so portable code needs no Linux headers).
constexpr uint32_t kErrClassBusOff = 0x00000040u;
constexpr uint32_t kErrClassRestarted = 0x00000100u;

enum class RecvStatus { Ok, Timeout, Closed };

class Link {
 public:
  virtual ~Link() = default;
  // Never blocks for long. False if the frame could not be queued (counted in tx_dropped).
  virtual bool send(const Frame& frame) = 0;
  // Waits up to timeout_ms (0 = just poll) for one frame that passes the filters.
  virtual RecvStatus recv(Frame& out, int timeout_ms) = 0;
  // Replaces the receive filters. An empty list accepts everything.
  virtual bool set_filters(const std::vector<Filter>& filters) = 0;
  virtual LinkStats stats() const = 0;
  // Wakes a blocked recv() (which returns Closed) and makes later calls fail.
  virtual void close() = 0;
};

// "ID#DATA" in cansend format: 8 hex digits mean an extended ID, 3 a standard one.
std::string to_cansend(const Frame& f);
bool from_cansend(const std::string& text, Frame& out, std::string* error);

}  // namespace anv
