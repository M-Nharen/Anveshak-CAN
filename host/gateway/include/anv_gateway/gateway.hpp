// The gateway core: owns the CAN bus, decodes every frame, tracks nodes, runs the leader
// election and, while master, the 20 ms control cycle. Plain C++17, no ROS.
//
// Threading: start() launches two threads, an RX thread (reads and decodes frames) and a
// control thread (absolute-deadline 10 ms tick: election timers, node timeouts, HOST_HB at 20 Hz,
// and SYNC + DRIVE_CMDs every 20 ms while master). Every public method is thread-safe and
// none throws.
#pragma once

#include <array>
#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "anv_gateway/can_bus.hpp"
#include "anv_gateway/config.hpp"
#include "anv_gateway/events.hpp"
#include "anv_gateway/leadership.hpp"
#include "anv_gateway/messages.hpp"
#include "anv_gateway/node_registry.hpp"
#include "anv_gateway/protocol.hpp"

namespace anv::gw {

enum class CmdResult { Ok, NotRunning, NotMaster, EstopLatched, BadArgument, SendFailed, Timeout };
const char* cmd_result_name(CmdResult r);

// Result of a parameter SERVICE request (GET/SET/SAVE/RESTORE_DEFAULTS): `status`/`value_raw`
// are only meaningful when `result == CmdResult::Ok` (a response came back from the node).
struct ServiceResult {
  CmdResult result = CmdResult::Ok;
  uint8_t status = 0;
  uint32_t value_raw = 0;
};

// Target for one drive node (two motor channels). Units: rad/s (or % duty in OpenLoopDuty mode).
struct WheelTarget {
  DriveMode mode = DriveMode::Velocity;
  double a = 0.0;
  double b = 0.0;
  double accel = 10.0;  // rad/s^2 limit sent with every command
};

struct BusStats {
  uint64_t rx_frames = 0;
  uint64_t tx_frames = 0;
  uint64_t tx_failures = 0;
  uint64_t error_frames = 0;
  uint64_t undecoded_frames = 0;  // IDs not in the DBC, or too short
  bool bus_off = false;           // last SocketCAN error class said bus-off and no restart since
  double rx_fps = 0;              // frames per second over the last full second
};

struct GatewayState {
  int64_t time_ns = 0;
  uint8_t host_id = 0;
  Role role = Role::Booting;
  uint16_t term = 0;
  uint8_t believed_master = 0;
  bool leader_lost = false;
  bool armed = false;  // this gateway sent ARM since it last claimed the lead
  bool estop_latched = false;
  uint64_t cycles_sent = 0;
  uint64_t late_cycles = 0;       // cycles that started more than 2 ms after their deadline
  int64_t worst_lateness_ns = 0;
  double cycle_hz = 50.0;         // control cycle (SYNC + DRIVE_CMD) rate: a gateway setting, 25-50 Hz
  double host_hb_hz = 20.0;       // this host's HOST_HB rate: a gateway setting, >= 20 Hz
  std::array<PeerHost, 4> peers{};  // indexed by host node (1 Orin, 2 RPi, 3 tool)
  BusStats bus;
  std::vector<NodeInfo> nodes;
  std::vector<RateInfo> rates;
};

using EventCallback = std::function<void(const GatewayEvent&)>;
using LogCallback = std::function<void(const std::string&)>;

class Gateway {
 public:
  Gateway(const GatewayConfig& cfg, std::unique_ptr<ICanBus> bus);
  ~Gateway();
  Gateway(const Gateway&) = delete;
  Gateway& operator=(const Gateway&) = delete;

  bool start();  // false if already started
  void stop();   // joins the threads; sends nothing (also how tests "kill" a host)
  bool running() const { return running_.load(); }

  // --- Drive targets (held until replaced; anything older than 100 ms is sent as zero speed) ---
  void set_targets(const std::array<WheelTarget, kNumDrives>& targets);
  CmdResult set_wheel_target(uint8_t drive /*1-6*/, const WheelTarget& target);

  // --- Master-only commands ---
  CmdResult arm();            // NMT ARM to all nodes
  CmdResult disarm();         // NMT DISARM to all nodes
  CmdResult release_estop();  // ESTOP_RELEASE; nodes go DISARMED

  // --- Any role ---
  CmdResult estop();                       // ESTOP frame, latched until released
  CmdResult request_lead(uint8_t host_id); // LEAD_REQUEST for host 1 (Orin) or 2 (RPi)

  // --- Parameters (SERVICE GET/SET/SAVE/RESTORE_DEFAULTS): node_id 4-9 (drive). Sends the
  // request and blocks (up to timeout_ms) for the matching response; any host may call this
  // (SERVICE is addressed per-host on the bus, not master-only). value_raw/the returned
  // value_raw are raw wire bits, reinterpreted per the parameter's type (docs/architecture.md,
  // defs/params/drive.yaml): the caller does that conversion, not the gateway.
  ServiceResult service_request(uint8_t node_id, ServiceOpcode op, uint16_t param_index, uint32_t value_raw = 0,
                                 int timeout_ms = 200);

  // --- Host-sent rates (gateway settings; docs/architecture.md "Rates page" safety floors) ---
  CmdResult set_cycle_rate_hz(double hz);    // control cycle (SYNC + DRIVE_CMD): 25-50 Hz
  CmdResult set_host_hb_rate_hz(double hz);  // this host's HOST_HB: 20-50 Hz

  GatewayState state() const;

  // Callbacks run on the gateway's own threads: keep them short and never call
  // subscribe/unsubscribe from inside one. Returns an ID for unsubscribe().
  int subscribe(EventCallback cb);
  void unsubscribe(int id);
  void set_logger(LogCallback cb);

 private:
  void rx_loop();
  void control_loop();
  void handle_frame(const CanFrame& f);
  void emit(const std::vector<GatewayEvent>& events);
  bool send(const CanFrame& f);
  void note_role_change_locked(Role old_role, uint16_t old_term);
  void apply_realtime(int priority_offset);
  void log(const std::string& msg);

  const GatewayConfig cfg_;
  std::unique_ptr<ICanBus> bus_;

  std::atomic<bool> running_{false};
  std::thread rx_thread_, control_thread_;
  int64_t start_ns_ = 0;

  mutable std::mutex mu_;  // guards everything below until the atomics
  LeaderElection election_;
  NodeRegistry registry_;
  struct TargetSlot {
    WheelTarget target;
    int64_t set_ns = 0;  // 0 = never set
  };
  std::array<TargetSlot, kNumDrives> targets_{};
  bool armed_ = false;
  bool estop_latched_ = false;
  uint8_t cycle_counter_ = 0;
  uint8_t lead_request_counter_ = 0;
  uint64_t cycles_sent_ = 0;
  uint64_t late_cycles_ = 0;
  int64_t worst_lateness_ns_ = 0;
  double rx_fps_ = 0;
  int64_t cycle_period_ns_ = kCyclePeriodNs;      // runtime copy of the (now adjustable) control cycle period
  int64_t host_hb_period_ns_ = kHostHbPeriodNs;    // runtime copy of this host's HOST_HB period
  int64_t last_cycle_ns_ = 0;                      // control thread only; read/written under mu_

  // Pending SERVICE requests, by transaction id, filled in by handle_frame() on the RX thread
  // and consumed by service_request() on whichever thread called it.
  std::condition_variable service_cv_;
  std::map<uint8_t, ServiceMsg> service_responses_;
  uint8_t service_txn_ = 0;

  std::atomic<uint64_t> rx_frames_{0}, tx_frames_{0}, tx_failures_{0}, error_frames_{0}, undecoded_{0};
  std::atomic<bool> bus_off_{false};

  std::mutex sub_mu_;
  std::vector<std::pair<int, EventCallback>> subscribers_;
  int next_sub_id_ = 1;
  LogCallback logger_;
  std::atomic<int64_t> last_late_log_ns_{0};
  std::atomic<bool> rt_warned_{false};
};

}  // namespace anv::gw
