// Frame decoder: raw CAN frame -> typed message, using the generated C unpack functions.
#pragma once

#include <cstdint>
#include <string_view>

#include "anv_gateway/can_bus.hpp"
#include "anv_gateway/messages.hpp"

namespace anv::gw {

struct Decoded {
  uint16_t id = 0;
  int64_t rx_ns = 0;
  Message msg;
};

// Returns false for an ID that is not in the DBC or a frame shorter than its message length.
bool decode_frame(const CanFrame& frame, Decoded& out);

std::string_view message_name(const Message& m);

}  // namespace anv::gw
