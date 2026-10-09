// Plain-text status output for the executable (and for logs).
#pragma once

#include <string>

#include "anv_gateway/gateway.hpp"

namespace anv::gw {

const char* role_name(Role r);
const char* node_state_name(uint8_t state);

// Multi-line, fixed layout, one node per line.
std::string format_status(const GatewayState& s);

}  // namespace anv::gw
