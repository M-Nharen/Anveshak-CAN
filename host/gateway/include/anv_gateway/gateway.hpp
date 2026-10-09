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
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <deque>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "anv_gateway/bus_errors.hpp"
#include "anv_gateway/can_bus.hpp"
#include "anv_gateway/config.hpp"
#include "anv_gateway/debug_levels.hpp"
#include "anv_gateway/events.hpp"
#include "anv_gateway/flight_recorder.hpp"
#include "anv_gateway/leadership.hpp"
#include "anv_gateway/node_registry.hpp"
#include "anv_gateway/protocol.hpp"

namespace anv::gw {

enum class CmdResult { Ok, NotRunning, NotMaster, EstopLatched, BadArgument, SendFailed, Refused };
const char* cmd_result_name(CmdResult r);

class GatewayLink;  // gateway_link.hpp; only a pointer crosses this header, so no include cycle

struct BusStats {
  uint64_t rx_frames = 0;
  uint64_t tx_frames = 0;
  uint64_t tx_failures = 0;
  uint64_t error_frames = 0;
  uint64_t undecoded_frames = 0;  // IDs not in the DBC, or too short
  bool bus_off = false;           // last SocketCAN error class said bus-off and no restart since
  double rx_fps = 0;              // frames per second over the last full second
  std::array<uint64_t, kBusErrorTypeCount> error_types{};  // indexed by BusErrorType
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
  std::array<PeerHost, 4> peers{};  // indexed by host node (1 Orin, 2 RPi, 3 tool)
  BusStats bus;
  std::vector<NodeInfo> nodes;
  std::vector<RateInfo> rates;

  uint8_t debug_level = 0;             // whole-rover level (a node can be above this on its own)
  bool debug_on = false;               // any node or the whole rover is above level 1
  int64_t debug_auto_off_ns = 0;       // time left on the soonest auto-off timer, 0 = none running
  bool flight_recorder_enabled = false;
  bool link_configured = false;
  bool link_up = false;                // only meaningful when link_configured
};

using EventCallback = std::function<void(const GatewayEvent&)>;
using LogCallback = std::function<void(const std::string&)>;

struct EventRecord {
  uint64_t seq = 0;
  GatewayEvent event;
};

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

  // --- Debug levels (docs: "Debug levels: switching debugging on and off") ---
  // target_node 0 = whole rover. Refused (CmdResult::Refused) if the projected bus load from the
  // DBC rates would exceed 50%. Transmits DEBUG_LEVEL only while master; any host may call this
  // (a standby host forwards the request to the leader over the gateway link, if attached).
  CmdResult set_debug_level(uint8_t target_node, uint8_t level, uint8_t category_mask = kDebugCategoryAll,
                             uint16_t auto_off_min = 0);

  // --- Flight recorder ("save last minute") ---
  // Writes the last 60 s of raw CAN to cfg_.flight_recorder_dir. Returns the path written, or ""
  // if disabled or the directory is not set. Also called automatically on a critical event.
  std::string save_flight_recording(const std::string& reason);

  // Attach the UDP link to the other host's gateway before start(). Not owned; the caller keeps
  // it alive at least until stop() returns. A gateway without a link just never forwards/receives
  // anything cross-host, which is safe (docs: "If the link drops, nothing unsafe happens").
  void attach_link(GatewayLink* link);

  // Events since `seq` (exclusive), oldest first, capped at `max`. seq 0 returns the most recent
  // `max` events. Used by the dashboard bridge so a browser can catch up after being away.
  std::vector<EventRecord> events_since(uint64_t seq, size_t max = 500) const;

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

  // Locked by the caller (mu_). Computes the projected bus-load fraction (0..1) if `target_node`
  // moved to `level`, everything else held at its last-reported level.
  double estimate_bus_load_locked(uint8_t target_node, uint8_t level) const;
  void on_link_targets(uint8_t from_host, const std::array<WheelTarget, kNumDrives>& targets);
  void on_link_debug(uint8_t from_host, const DebugLevelMsg& msg);

  const GatewayConfig cfg_;
  std::unique_ptr<ICanBus> bus_;

  std::atomic<bool> running_{false};
  std::thread rx_thread_, control_thread_;
  int64_t start_ns_ = 0;
  GatewayLink* link_ = nullptr;
  std::FILE* event_log_file_ = nullptr;

  mutable std::mutex mu_;  // guards everything below until the atomics
  LeaderElection election_;
  NodeRegistry registry_;
  DebugLevelManager debug_;
  FlightRecorder flight_recorder_;
  BusErrorStats bus_errors_;
  std::map<uint8_t, uint8_t> last_event_seq_;  // node_id -> last NODE_EVENT seq seen, for gap detection
  uint8_t service_txn_counter_ = 0;
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
  int64_t last_link_alive_ns_ = 0;

  std::atomic<uint64_t> rx_frames_{0}, tx_frames_{0}, tx_failures_{0}, error_frames_{0}, undecoded_{0};
  std::atomic<bool> bus_off_{false};

  std::mutex sub_mu_;
  std::vector<std::pair<int, EventCallback>> subscribers_;
  int next_sub_id_ = 1;
  LogCallback logger_;
  std::atomic<int64_t> last_late_log_ns_{0};
  std::atomic<bool> rt_warned_{false};

  mutable std::mutex event_log_mu_;
  std::deque<EventRecord> event_log_;
  uint64_t next_event_seq_ = 1;
  static constexpr size_t kEventLogRingSize = 2000;
};

}  // namespace anv::gw
