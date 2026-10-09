// Decodes Linux SocketCAN error frames into the error types docs/architecture.md's "Decoding CAN
// bus errors" table explains, and raises BUS_ERROR_BURST when more than 10 arrive in a second.
#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <vector>

#include "anv_gateway/can_bus.hpp"
#include "anv_gateway/events.hpp"

namespace anv::gw {

enum class BusErrorType : uint8_t { Other = 0, Ack, Stuff, Form, Bit };
constexpr size_t kBusErrorTypeCount = 5;

BusErrorType classify_bus_error(const CanFrame& err_frame);
const char* bus_error_type_name(BusErrorType t);
// Matches the "What each error type usually means" table (gives the most common cause; the
// dashboard shows the fuller text).
const char* bus_error_likely_cause(BusErrorType t);

class BusErrorStats {
 public:
  void on_error_frame(const CanFrame& f, int64_t now_ns, std::vector<GatewayEvent>& events);
  const std::array<uint64_t, kBusErrorTypeCount>& totals() const { return totals_; }

 private:
  std::array<uint64_t, kBusErrorTypeCount> totals_{};
  int64_t window_start_ns_ = 0;
  uint32_t window_count_ = 0;
  bool window_flagged_ = false;  // already raised BUS_ERROR_BURST for the current window
};

}  // namespace anv::gw
