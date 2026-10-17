// Local control/state bridge for other processes on the same host (the dashboard backend today).
// A Unix stream socket carrying one JSON object per line, request -> one reply line:
//
//   {"id":1,"cmd":"state"}                              -> {"id":1,"ok":true,"state":{...}}
//   {"id":2,"cmd":"set_targets","wheels":[{"mode":2,"a":1.5,"b":1.5,"accel":10}, ...6]}
//   {"id":3,"cmd":"arm"} | "disarm" | "estop" | "release_estop"
//   {"id":4,"cmd":"request_lead","host":1}
//   {"id":5,"cmd":"ping"}
//   {"id":6,"cmd":"forget_node","node_id":17}          (free an auto-assigned ID in the table)
//   {"id":7,"cmd":"send_text","node_id":16,"text":"hello"}  (node 0 = every node; reply carries "seq")
//
// Command replies are {"id":N,"ok":true|false,"code":<CmdResult>,"result":"<text>"}. It only wraps the
// public Gateway API, so every rule (master-only commands, E-stop latch, 100 ms target hold)
// stays in the gateway. The socket is chmod 0660: access is by file permission.
#pragma once

#include <atomic>
#include <string>
#include <thread>

#include "anv_gateway/gateway.hpp"

namespace anv::gw {

// State as one JSON object (also used by tests).
std::string state_to_json(const GatewayState& s);

class Bridge {
 public:
  Bridge(Gateway& gateway, std::string socket_path);
  ~Bridge();
  Bridge(const Bridge&) = delete;
  Bridge& operator=(const Bridge&) = delete;

  bool start(std::string* error);  // false if the socket cannot be created
  void stop();

 private:
  void loop();
  std::string handle(const std::string& line);

  Gateway& gw_;
  const std::string path_;
  int listen_fd_ = -1;
  std::atomic<bool> running_{false};
  std::thread thread_;
};

}  // namespace anv::gw
