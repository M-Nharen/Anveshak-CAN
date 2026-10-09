// CAN I/O abstraction. The gateway talks only to this interface, so tests run on the
// in-process FakeBus and the real executable runs on SocketCAN.
#pragma once

#include <array>
#include <cstdint>

namespace anv::gw {

struct CanFrame {
  uint16_t id = 0;  // 11-bit identifier
  uint8_t dlc = 0;
  std::array<uint8_t, 8> data{};
  int64_t rx_ns = 0;  // CLOCK_MONOTONIC time the frame reached the host (kernel timestamp on SocketCAN)
  bool is_error = false;  // SocketCAN error frame: err_class / data carry the kernel's description
  uint32_t err_class = 0;
};

enum class RecvStatus { Ok, Timeout, Closed };

class ICanBus {
 public:
  virtual ~ICanBus() = default;
  // Thread-safe, never blocks for long. Returns false if the frame could not be queued.
  virtual bool send(const CanFrame& frame) = 0;
  // Waits up to timeout_ms for one frame.
  virtual RecvStatus recv(CanFrame& out, int timeout_ms) = 0;
  // Wakes a blocked recv() (which then returns Closed) and makes later calls fail.
  virtual void close() = 0;
};

// SocketCAN error-frame class bits we care about (values from linux/can/error.h; copied so the
// portable code does not need Linux headers).
constexpr uint32_t kErrClassBusOff = 0x00000040u;
constexpr uint32_t kErrClassRestarted = 0x00000100u;

}  // namespace anv::gw
