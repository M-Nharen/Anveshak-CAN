// Flight recorder: docs/architecture.md "Debug levels" / "Flight recorder (separate switch, on
// by default)". Keeps the last 60 s of raw CAN traffic in memory and writes it to disk only when
// asked to -- on a critical event, or "save last minute" from the dashboard -- so a crash nobody
// had debugging on for still leaves a trail.
#pragma once

#include <cstddef>
#include <cstdint>
#include <deque>
#include <mutex>
#include <string>

#include "anv_gateway/can_bus.hpp"
#include "anv_gateway/protocol.hpp"

namespace anv::gw {

class FlightRecorder {
 public:
  explicit FlightRecorder(int64_t window_ns = 60 * kNsPerSec) : window_ns_(window_ns) {}

  // Every frame goes in, including error frames; frames older than the window (by f.rx_ns,
  // relative to the newest one recorded) are dropped as new ones arrive.
  void record(const CanFrame& f);

  size_t size() const;
  void clear();

  // Writes the buffer to "<dir>/flight_<unix-ms>_<reason>.log" (candump-style text, oldest
  // first) and returns the path, or "" if dir is empty or the file could not be created.
  std::string save(const std::string& dir, const std::string& reason) const;

 private:
  int64_t window_ns_;
  mutable std::mutex mu_;
  std::deque<CanFrame> frames_;
};

}  // namespace anv::gw
