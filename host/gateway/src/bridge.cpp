#include "anv_gateway/bridge.hpp"

#include <poll.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <unistd.h>

#include <cerrno>
#include <cstring>
#include <map>

#include "anv_gateway/messages.hpp"
#include "anv_gateway/status.hpp"
#include "json_mini.hpp"

namespace anv::gw {

namespace {

constexpr size_t kMaxLine = 64 * 1024;

std::string b(bool v) { return v ? "true" : "false"; }

std::string node_json(const NodeInfo& n, int64_t now_ns) {
  using json::num;
  using json::quote;
  std::string o = "{\"node_id\":" + std::to_string(n.node_id) + ",\"name\":" + quote(n.name) +
                  ",\"seen\":" + b(n.seen) + ",\"alive\":" + b(n.alive) +
                  ",\"age_ms\":" + (n.seen ? num((now_ns - n.last_seen_ns) / 1e6) : std::string("null")) +
                  ",\"state\":" + std::to_string(n.state) + ",\"state_name\":" + quote(node_state_name(n.state)) +
                  ",\"has_hb\":" + b(n.has_hb) + ",\"node_type\":" + std::to_string(n.node_type) +
                  ",\"fw_build_id\":" + std::to_string(n.fw_build_id) + ",\"uptime_s\":" + std::to_string(n.uptime_s) +
                  ",\"reboot_count\":" + std::to_string(n.reboot_count) + ",\"tec\":" + std::to_string(n.tec) +
                  ",\"rec\":" + std::to_string(n.rec) + ",\"can_error_state\":" + std::to_string(n.can_error_state) +
                  ",\"bootloader_running\":" + b(n.bootloader_running) + ",\"debug_level\":" + std::to_string(n.debug_level);
  if (n.has_fb_a)
    o += ",\"fb\":{\"state\":" + std::to_string(n.fb_a.state) + ",\"fault_flags\":" + std::to_string(n.fb_a.fault_flags) +
         ",\"speed_a\":" + num(speed_rad_s(n.fb_a.speed_a)) + ",\"speed_b\":" + num(speed_rad_s(n.fb_a.speed_b)) +
         ",\"current_a\":" + num(current_a(n.fb_a.current_a)) + ",\"current_b\":" + num(current_a(n.fb_a.current_b)) + "}";
  if (n.has_telem0)
    o += ",\"telem\":{\"supply_v\":" + num(n.telem0.supply_voltage_mv / 1000.0) +
         ",\"temp_driver_a\":" + std::to_string(n.telem0.temp_driver_a) +
         ",\"temp_driver_b\":" + std::to_string(n.telem0.temp_driver_b) +
         ",\"temp_mcu\":" + std::to_string(n.telem0.temp_mcu) + "}";
  return o + "}";
}

}  // namespace

std::string state_to_json(const GatewayState& s) {
  using json::num;
  std::string o = "{\"time_ns\":" + std::to_string(s.time_ns) + ",\"host_id\":" + std::to_string(s.host_id) +
                  ",\"role\":" + std::to_string(static_cast<int>(s.role)) + ",\"role_name\":" + json::quote(role_name(s.role)) +
                  ",\"term\":" + std::to_string(s.term) + ",\"believed_master\":" + std::to_string(s.believed_master) +
                  ",\"leader_lost\":" + b(s.leader_lost) + ",\"armed\":" + b(s.armed) +
                  ",\"estop_latched\":" + b(s.estop_latched) + ",\"cycles_sent\":" + std::to_string(s.cycles_sent) +
                  ",\"late_cycles\":" + std::to_string(s.late_cycles) +
                  ",\"worst_lateness_us\":" + std::to_string(s.worst_lateness_ns / 1000) +
                  ",\"cycle_hz\":" + num(s.cycle_hz) + ",\"host_hb_hz\":" + num(s.host_hb_hz) +
                  ",\"bus\":{\"rx_frames\":" + std::to_string(s.bus.rx_frames) + ",\"tx_frames\":" + std::to_string(s.bus.tx_frames) +
                  ",\"tx_failures\":" + std::to_string(s.bus.tx_failures) + ",\"error_frames\":" + std::to_string(s.bus.error_frames) +
                  ",\"undecoded_frames\":" + std::to_string(s.bus.undecoded_frames) + ",\"bus_off\":" + b(s.bus.bus_off) +
                  ",\"rx_fps\":" + num(s.bus.rx_fps) + "},\"peers\":[";
  for (size_t i = 1; i < s.peers.size(); ++i) {  // index = host node: 1 Orin, 2 RPi, 3 tool
    if (i > 1) o += ",";
    o += "{\"host\":" + std::to_string(i) + ",\"seen\":" + b(s.peers[i].seen) + ",\"role\":" + std::to_string(s.peers[i].role) +
         ",\"term\":" + std::to_string(s.peers[i].term) + "}";
  }
  o += "],\"nodes\":[";
  for (size_t i = 0; i < s.nodes.size(); ++i) {
    if (i) o += ",";
    o += node_json(s.nodes[i], s.time_ns);
  }
  return o + "]}";
}

Bridge::Bridge(Gateway& gateway, std::string socket_path) : gw_(gateway), path_(std::move(socket_path)) {}

Bridge::~Bridge() { stop(); }

bool Bridge::start(std::string* error) {
  sockaddr_un addr{};
  if (path_.empty() || path_.size() >= sizeof addr.sun_path) {
    if (error) *error = "bridge socket path is empty or too long";
    return false;
  }
  listen_fd_ = ::socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
  if (listen_fd_ < 0) {
    if (error) *error = std::string("socket: ") + std::strerror(errno);
    return false;
  }
  addr.sun_family = AF_UNIX;
  std::strncpy(addr.sun_path, path_.c_str(), sizeof addr.sun_path - 1);
  ::unlink(path_.c_str());  // a stale socket from a previous run
  if (::bind(listen_fd_, reinterpret_cast<sockaddr*>(&addr), sizeof addr) != 0 || ::listen(listen_fd_, 8) != 0) {
    if (error) *error = "cannot listen on " + path_ + ": " + std::strerror(errno);
    ::close(listen_fd_);
    listen_fd_ = -1;
    return false;
  }
  ::chmod(path_.c_str(), 0660);
  running_ = true;
  thread_ = std::thread([this] { loop(); });
  return true;
}

void Bridge::stop() {
  if (!running_.exchange(false)) return;
  if (thread_.joinable()) thread_.join();
  if (listen_fd_ >= 0) ::close(listen_fd_);
  listen_fd_ = -1;
  ::unlink(path_.c_str());
}

void Bridge::loop() {
  std::map<int, std::string> clients;  // fd -> partial line
  while (running_) {
    std::vector<pollfd> fds{{listen_fd_, POLLIN, 0}};
    for (auto& c : clients) fds.push_back({c.first, POLLIN, 0});
    if (::poll(fds.data(), fds.size(), 20) <= 0) continue;

    if (fds[0].revents & POLLIN) {
      int fd = ::accept4(listen_fd_, nullptr, nullptr, SOCK_CLOEXEC | SOCK_NONBLOCK);
      if (fd >= 0) clients[fd];
    }
    for (size_t i = 1; i < fds.size(); ++i) {
      if (!(fds[i].revents & (POLLIN | POLLHUP | POLLERR))) continue;
      const int fd = fds[i].fd;
      char buf[4096];
      const ssize_t n = ::recv(fd, buf, sizeof buf, 0);
      bool drop = n == 0 || (n < 0 && errno != EAGAIN && errno != EINTR);
      if (n > 0) {
        std::string& pending = clients[fd];
        pending.append(buf, static_cast<size_t>(n));
        size_t nl;
        while (!drop && (nl = pending.find('\n')) != std::string::npos) {
          std::string reply = handle(pending.substr(0, nl)) + "\n";
          pending.erase(0, nl + 1);
          // Replies are small and the socket is local; a client that cannot take one is dropped.
          if (::send(fd, reply.data(), reply.size(), MSG_NOSIGNAL) != static_cast<ssize_t>(reply.size())) drop = true;
        }
        if (pending.size() > kMaxLine) drop = true;
      }
      if (drop) {
        ::close(fd);
        clients.erase(fd);
      }
    }
  }
  for (auto& c : clients) ::close(c.first);
}

std::string Bridge::handle(const std::string& line) {
  json::Value req;
  if (!json::Parser(line).parse(req) || req.type != json::Value::Object)
    return "{\"id\":null,\"ok\":false,\"error\":\"bad json\"}";
  const json::Value* idv = req.get("id");
  const std::string id = (idv && idv->type == json::Value::Number) ? json::num(idv->num) : "null";
  auto fail = [&](const std::string& msg) { return "{\"id\":" + id + ",\"ok\":false,\"error\":" + json::quote(msg) + "}"; };
  auto result = [&](CmdResult r) {
    return "{\"id\":" + id + ",\"ok\":" + b(r == CmdResult::Ok) + ",\"code\":" + std::to_string(static_cast<int>(r)) +
           ",\"result\":" + json::quote(cmd_result_name(r)) + "}";
  };

  const json::Value* cmdv = req.get("cmd");
  if (!cmdv || cmdv->type != json::Value::String) return fail("missing cmd");
  const std::string& cmd = cmdv->str;

  if (cmd == "ping") return "{\"id\":" + id + ",\"ok\":true}";
  if (cmd == "state") return "{\"id\":" + id + ",\"ok\":true,\"state\":" + state_to_json(gw_.state()) + "}";
  if (cmd == "arm") return result(gw_.arm());
  if (cmd == "disarm") return result(gw_.disarm());
  if (cmd == "estop") return result(gw_.estop());
  if (cmd == "release_estop") return result(gw_.release_estop());
  if (cmd == "request_lead") {
    const json::Value* h = req.get("host");
    if (!h || h->type != json::Value::Number || (h->num != 1 && h->num != 2)) return fail("host must be 1 or 2");
    return result(gw_.request_lead(static_cast<uint8_t>(h->num)));
  }
  if (cmd == "get_param" || cmd == "set_param" || cmd == "save_param" || cmd == "restore_param") {
    const json::Value* nv = req.get("node");
    const json::Value* iv = req.get("index");
    if (!nv || nv->type != json::Value::Number || !iv || iv->type != json::Value::Number)
      return fail("node and index required");
    uint32_t value_raw = 0;
    ServiceOpcode op = ServiceOpcode::Get;
    if (cmd == "set_param") {
      const json::Value* vv = req.get("value_raw");
      if (!vv || vv->type != json::Value::Number) return fail("value_raw required");
      value_raw = static_cast<uint32_t>(vv->num);
      op = ServiceOpcode::Set;
    } else if (cmd == "save_param") {
      op = ServiceOpcode::Save;
    } else if (cmd == "restore_param") {
      op = ServiceOpcode::RestoreDefaults;
    }
    const ServiceResult r = gw_.service_request(static_cast<uint8_t>(nv->num), op, static_cast<uint16_t>(iv->num), value_raw);
    return "{\"id\":" + id + ",\"ok\":" + b(r.result == CmdResult::Ok && r.status == 0) +
           ",\"result\":" + json::quote(cmd_result_name(r.result)) + ",\"status\":" + std::to_string(r.status) +
           ",\"value_raw\":" + std::to_string(r.value_raw) + "}";
  }
  if (cmd == "set_host_rate") {
    const json::Value* cv = req.get("cycle_hz");
    const json::Value* hv = req.get("host_hb_hz");
    CmdResult r = CmdResult::Ok;
    if (cv && cv->type == json::Value::Number) r = gw_.set_cycle_rate_hz(cv->num);
    if (r == CmdResult::Ok && hv && hv->type == json::Value::Number) r = gw_.set_host_hb_rate_hz(hv->num);
    return result(r);
  }
  if (cmd == "set_targets") {
    const json::Value* w = req.get("wheels");
    if (!w || w->type != json::Value::Array || w->arr.size() != kNumDrives) return fail("wheels must be an array of 6");
    std::array<WheelTarget, kNumDrives> t{};
    for (size_t i = 0; i < kNumDrives; ++i) {
      const json::Value& e = w->arr[i];
      if (e.type != json::Value::Object) return fail("each wheel must be an object");
      auto number = [&](const char* k, double dflt) {
        const json::Value* v = e.get(k);
        return (v && v->type == json::Value::Number) ? v->num : dflt;
      };
      const double mode = number("mode", 2);
      if (mode < 0 || mode > 3) return fail("mode must be 0-3");
      t[i].mode = static_cast<DriveMode>(static_cast<int>(mode));
      t[i].a = number("a", 0);
      t[i].b = number("b", 0);
      t[i].accel = number("accel", 10);
    }
    gw_.set_targets(t);
    return "{\"id\":" + id + ",\"ok\":true,\"result\":\"Ok\"}";
  }
  return fail("unknown cmd '" + cmd + "'");
}

}  // namespace anv::gw
