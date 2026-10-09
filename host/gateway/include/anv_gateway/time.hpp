// Monotonic time helpers. All gateway timing uses CLOCK_MONOTONIC so wall-clock jumps
// (NTP, manual set) can never trip a watchdog or stretch the 20 ms cycle.
#pragma once

#include <cstdint>
#include <ctime>

namespace anv::gw {

constexpr int64_t kNsPerMs = 1000000LL;
constexpr int64_t kNsPerSec = 1000000000LL;

inline int64_t mono_ns() {
  timespec ts{};
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return static_cast<int64_t>(ts.tv_sec) * kNsPerSec + ts.tv_nsec;
}

}  // namespace anv::gw
