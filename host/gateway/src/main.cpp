// anv_can_gateway: owns can0 on this host. Text commands on stdin make it scriptable and
// handy at the desk; the ROS 2 and dashboard layers (later work) use the C++ API instead.
#include <poll.h>
#include <signal.h>
#include <unistd.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <sstream>
#include <string>

#include "anv_gateway/bridge.hpp"
#include "anv_gateway/config.hpp"
#include "anv_gateway/gateway.hpp"
#include "anv_gateway/socketcan_bus.hpp"
#include "anv_gateway/status.hpp"

using namespace anv::gw;

namespace {

volatile sig_atomic_t g_stop = 0;
void on_signal(int) { g_stop = 1; }

void usage(const char* argv0) {
  std::printf(
      "usage: %s [--config FILE] [--interface IFACE] [--status-interval SECONDS]\n"
      "\n"
      "  --config FILE            flat YAML: host_id, tie_break, auto_takeover, interface,\n"
      "                           rt_priority, cpu_affinity, bridge_socket (see config/gateway.example.yaml)\n"
      "  --interface IFACE        CAN interface, overrides the config file (default can0)\n"
      "  --bridge-socket PATH     serve the dashboard bridge on this Unix socket (JSON lines)\n"
      "  --status-interval SEC    print status every SEC seconds, 0 = never (default 1)\n"
      "\n"
      "commands on stdin:\n"
      "  lead 1|2        ask host 1 (Orin) or 2 (RPi) to lead (LEAD_REQUEST on the bus)\n"
      "  arm | disarm    NMT ARM / DISARM to all nodes (master only)\n"
      "  drive A B       send A, B (rad/s) to every drive node until 'stop' (master only matters)\n"
      "  stop            stop driving (targets go stale, zero speed follows within 100 ms)\n"
      "  estop           send E-stop           release   release E-stop (master only)\n"
      "  status          print status now      quit      exit\n",
      argv0);
}

const char* describe(CmdResult r) { return cmd_result_name(r); }

}  // namespace

int main(int argc, char** argv) {
  GatewayConfig cfg;
  std::string config_path, iface_override, bridge_override;
  double status_interval = 1.0;

  for (int i = 1; i < argc; ++i) {
    const std::string a = argv[i];
    auto next = [&](const char* name) -> const char* {
      if (i + 1 >= argc) {
        std::fprintf(stderr, "%s needs a value\n", name);
        std::exit(2);
      }
      return argv[++i];
    };
    if (a == "--config") config_path = next("--config");
    else if (a == "--interface") iface_override = next("--interface");
    else if (a == "--bridge-socket") bridge_override = next("--bridge-socket");
    else if (a == "--status-interval") status_interval = std::atof(next("--status-interval"));
    else if (a == "--help" || a == "-h") { usage(argv[0]); return 0; }
    else { std::fprintf(stderr, "unknown argument '%s'\n", a.c_str()); usage(argv[0]); return 2; }
  }

  std::string err;
  if (!config_path.empty() && !load_config_file(config_path, cfg, &err)) {
    std::fprintf(stderr, "config error: %s\n", err.c_str());
    return 2;
  }
  if (!iface_override.empty()) cfg.interface_name = iface_override;
  if (!bridge_override.empty()) cfg.bridge_socket = bridge_override;

  auto bus = SocketCanBus::open(cfg.interface_name, &err);
  if (!bus) {
    std::fprintf(stderr, "cannot open %s: %s\n", cfg.interface_name.c_str(), err.c_str());
    return 1;
  }

  Gateway gw(cfg, std::move(bus));
  gw.set_logger([](const std::string& m) { std::fprintf(stderr, "gateway: %s\n", m.c_str()); });
  gw.subscribe([](const GatewayEvent& e) {
    if (e.type == EventType::NodeIdAssigned)
      std::printf("event %s node %u mac %s\n", event_type_name(e.type), e.node_id, IdAllocator::mac_to_string(e.mac).c_str());
    else
      std::printf("event %s node %u term %u role %u\n", event_type_name(e.type), e.node_id, e.term, e.role);
    std::fflush(stdout);
  });

  struct sigaction sa{};
  sa.sa_handler = on_signal;
  sigaction(SIGINT, &sa, nullptr);
  sigaction(SIGTERM, &sa, nullptr);

  if (!gw.start()) return 1;
  std::printf("anv_can_gateway: host %u on %s, standby until a lead request names this host\n", cfg.host_id,
              cfg.interface_name.c_str());
  std::fflush(stdout);

  // The dashboard backend talks to the gateway through this socket (host/gateway/README.md).
  Bridge bridge(gw, cfg.bridge_socket);
  if (!cfg.bridge_socket.empty()) {
    if (!bridge.start(&err)) {
      std::fprintf(stderr, "bridge: %s\n", err.c_str());
      gw.stop();
      return 1;
    }
    std::printf("dashboard bridge on %s\n", cfg.bridge_socket.c_str());
    std::fflush(stdout);
  }

  bool stdin_open = true, driving = false;
  double drive_a = 0, drive_b = 0;
  std::string pending;
  int64_t last_status = mono_ns();

  while (!g_stop) {
    if (stdin_open) {
      pollfd pfd{STDIN_FILENO, POLLIN, 0};
      poll(&pfd, 1, 20);
      if (pfd.revents & (POLLIN | POLLHUP)) {
        char buf[256];
        const ssize_t n = read(STDIN_FILENO, buf, sizeof buf);
        if (n > 0) pending.append(buf, static_cast<size_t>(n));
        else if (n == 0) stdin_open = false;  // e.g. run as a service: keep running, just no commands
      }
    } else {
      poll(nullptr, 0, 20);
    }

    size_t nl;
    while ((nl = pending.find('\n')) != std::string::npos) {
      std::istringstream line(pending.substr(0, nl));
      pending.erase(0, nl + 1);
      std::string cmd;
      line >> cmd;
      if (cmd.empty()) continue;
      if (cmd == "quit" || cmd == "exit") { g_stop = 1; break; }
      else if (cmd == "lead") { int h = 0; line >> h; std::printf("lead %d: %s\n", h, describe(gw.request_lead(static_cast<uint8_t>(h)))); }
      else if (cmd == "arm") std::printf("arm: %s\n", describe(gw.arm()));
      else if (cmd == "disarm") std::printf("disarm: %s\n", describe(gw.disarm()));
      else if (cmd == "estop") { driving = false; std::printf("estop: %s\n", describe(gw.estop())); }
      else if (cmd == "release") std::printf("release: %s\n", describe(gw.release_estop()));
      else if (cmd == "drive") {
        if (line >> drive_a >> drive_b) { driving = true; std::printf("driving all wheels at %.2f / %.2f rad/s\n", drive_a, drive_b); }
        else std::printf("usage: drive A B\n");
      }
      else if (cmd == "stop") { driving = false; std::printf("stopped\n"); }
      else if (cmd == "status") std::printf("%s", format_status(gw.state()).c_str());
      else std::printf("unknown command '%s' (try: lead arm disarm drive stop estop release status quit)\n", cmd.c_str());
      std::fflush(stdout);
    }

    // Targets go stale after 100 ms by design, so a held 'drive' has to be refreshed continuously.
    if (driving) {
      WheelTarget t;
      t.a = drive_a;
      t.b = drive_b;
      std::array<WheelTarget, kNumDrives> all;
      all.fill(t);
      gw.set_targets(all);
    }

    if (status_interval > 0 && mono_ns() - last_status >= static_cast<int64_t>(status_interval * kNsPerSec)) {
      last_status = mono_ns();
      std::printf("---\n%s", format_status(gw.state()).c_str());
      std::fflush(stdout);
    }
  }

  bridge.stop();
  gw.stop();
  return 0;
}
