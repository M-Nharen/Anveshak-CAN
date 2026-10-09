// Gateway configuration. The file format is a flat subset of YAML ("key: value" lines, # comments),
// so the gateway needs no YAML library on any host.
#pragma once

#include <cstdint>
#include <string>

namespace anv::gw {

struct GatewayConfig {
  std::string interface_name = "can0";
  uint8_t host_id = 1;      // 1 Orin, 2 RPi
  uint8_t tie_break = 0;    // 0 = use host_id (Orin 1, RPi 2)
  bool auto_takeover = false;
  int rt_priority = 50;     // SCHED_FIFO priority for the CAN threads; 0 = leave scheduling alone
  std::string bridge_socket;  // Unix socket for the dashboard backend; empty = no bridge
  int cpu_affinity = -1;    // pin the CAN threads to this core (RPi); -1 = no pinning

  // Flight recorder (docs: "Flight recorder (separate switch, on by default)"). Empty dir = the
  // ring buffer is still kept in memory, but save() has nowhere to write.
  bool flight_recorder = true;
  std::string flight_recorder_dir;

  // Event log file, one JSON line per event (docs: "event log file from level 1"). Empty = off.
  std::string event_log_dir;

  // The UDP link to the other host's gateway (docs: "The gateway link"). Off by default: a
  // single-host setup (bench testing, CI) needs neither port.
  bool link_enabled = false;
  uint16_t link_listen_port = 0;
  std::string link_peer_host;
  uint16_t link_peer_port = 0;

  uint8_t effective_tie_break() const { return tie_break != 0 ? tie_break : host_id; }
};

// No exceptions: both return false and fill *error ("line N: ...") on failure.
bool parse_config_text(const std::string& text, GatewayConfig& out, std::string* error);
bool load_config_file(const std::string& path, GatewayConfig& out, std::string* error);

}  // namespace anv::gw
