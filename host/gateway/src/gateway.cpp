#include "anv_gateway/gateway.hpp"

#include <algorithm>
#include <cerrno>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <ctime>
#include <type_traits>

#include <sys/stat.h>

#include "anv_gateway/encoder.hpp"
#include "anv_gateway/gateway_link.hpp"
#include "errors.h"
#include "ids.h"

#ifdef __linux__
#include <pthread.h>
#include <sched.h>
#endif

namespace anv::gw {

namespace {

ElectionConfig election_config(const GatewayConfig& c) {
  ElectionConfig e;
  e.host_id = c.host_id;
  e.tie_break = c.effective_tie_break();
  e.auto_takeover = c.auto_takeover;
  return e;
}

// Calibrated so that six drive nodes + the FPGA + two hosts at debug level 1 come out close to
// the ~31% baseline load docs/architecture.md measures on the real rover; it is an estimate for
// the bus-load guard, not a bit-exact model of CAN's stuffing and arbitration overhead.
constexpr double kLinkBitsPerFrame = 121.0;
constexpr double kCanBitrate = 500000.0;

void write_event_log_line(std::FILE* f, const GatewayEvent& e) {
  if (!f) return;
  std::fprintf(f,
               "{\"time_ns\":%lld,\"type\":\"%s\",\"node_id\":%u,\"code\":%u,\"severity\":%u,"
               "\"detail\":%u,\"cleared\":%s}\n",
               static_cast<long long>(e.time_ns), event_type_name(e.type), e.node_id, e.code, e.severity,
               e.detail, e.cleared ? "true" : "false");
  std::fflush(f);
}

int16_t scale_target(double v) {
  const double raw = std::round(v * kTargetScale);
  return static_cast<int16_t>(std::clamp(raw, -32768.0, 32767.0));
}

uint16_t scale_accel(double v) {
  const double raw = std::round(v * kTargetScale);
  return static_cast<uint16_t>(std::clamp(raw, 0.0, 65535.0));
}

void sleep_until_ns(int64_t target_ns) {
  timespec ts;
  ts.tv_sec = static_cast<time_t>(target_ns / kNsPerSec);
  ts.tv_nsec = static_cast<long>(target_ns % kNsPerSec);
  // Absolute deadline on the monotonic clock: a relative sleep(20 ms) in a loop would drift.
  while (clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME, &ts, nullptr) == EINTR) {
  }
}

}  // namespace

const char* cmd_result_name(CmdResult r) {
  switch (r) {
    case CmdResult::Ok: return "ok";
    case CmdResult::NotRunning: return "gateway not running";
    case CmdResult::NotMaster: return "this host is not the master";
    case CmdResult::EstopLatched: return "E-stop is latched";
    case CmdResult::BadArgument: return "bad argument";
    case CmdResult::SendFailed: return "CAN send failed";
    case CmdResult::Refused: return "refused: would exceed the bus-load guard";
  }
  return "?";
}

Gateway::Gateway(const GatewayConfig& cfg, std::unique_ptr<ICanBus> bus)
    : cfg_(cfg),
      bus_(std::move(bus)),
      election_(election_config(cfg)),
      debug_([this](uint8_t node, uint8_t level) { return estimate_bus_load_locked(node, level); }),
      flight_recorder_() {}

Gateway::~Gateway() { stop(); }

bool Gateway::start() {
  bool expected = false;
  if (!running_.compare_exchange_strong(expected, true)) return false;
  start_ns_ = mono_ns();
  {
    std::lock_guard<std::mutex> lk(mu_);
    election_.start(start_ns_);  // standby: a booting host never claims the lead by itself
  }
  if (!cfg_.event_log_dir.empty()) {
    ::mkdir(cfg_.event_log_dir.c_str(), 0755);  // best effort; fopen below surfaces a real failure
    const auto now_ms =
        std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch())
            .count();
    const std::string path = cfg_.event_log_dir + "/events_" + std::to_string(now_ms) + ".jsonl";
    event_log_file_ = std::fopen(path.c_str(), "w");
  }
  rx_thread_ = std::thread([this] { rx_loop(); });
  control_thread_ = std::thread([this] { control_loop(); });
  return true;
}

void Gateway::stop() {
  if (!running_.exchange(false)) return;
  bus_->close();
  if (rx_thread_.joinable()) rx_thread_.join();
  if (control_thread_.joinable()) control_thread_.join();
  if (event_log_file_) {
    std::fclose(event_log_file_);
    event_log_file_ = nullptr;
  }
}

void Gateway::attach_link(GatewayLink* link) {
  link_ = link;
  if (!link_) return;
  link_->set_on_targets(
      [this](uint8_t from_host, const std::array<WheelTarget, kNumDrives>& t) { on_link_targets(from_host, t); });
  link_->set_on_debug([this](uint8_t from_host, const DebugLevelMsg& m) { on_link_debug(from_host, m); });
}

void Gateway::on_link_targets(uint8_t /*from_host*/, const std::array<WheelTarget, kNumDrives>& t) {
  std::lock_guard<std::mutex> lk(mu_);
  if (!election_.is_master()) return;  // only the leader actually drives from forwarded targets
  const int64_t now = mono_ns();
  for (size_t i = 0; i < kNumDrives; ++i) targets_[i] = {t[i], now};
}

void Gateway::on_link_debug(uint8_t /*from_host*/, const DebugLevelMsg& m) {
  set_debug_level(m.target_node, m.level, m.category_mask, m.auto_off_timer_min);
}

void Gateway::set_logger(LogCallback cb) {
  std::lock_guard<std::mutex> lk(sub_mu_);
  logger_ = std::move(cb);
}

void Gateway::log(const std::string& msg) {
  LogCallback cb;
  {
    std::lock_guard<std::mutex> lk(sub_mu_);
    cb = logger_;
  }
  if (cb) cb(msg);
}

int Gateway::subscribe(EventCallback cb) {
  std::lock_guard<std::mutex> lk(sub_mu_);
  const int id = next_sub_id_++;
  subscribers_.emplace_back(id, std::move(cb));
  return id;
}

void Gateway::unsubscribe(int id) {
  std::lock_guard<std::mutex> lk(sub_mu_);
  subscribers_.erase(std::remove_if(subscribers_.begin(), subscribers_.end(),
                                    [id](const auto& p) { return p.first == id; }),
                     subscribers_.end());
}

void Gateway::emit(const std::vector<GatewayEvent>& events) {
  if (events.empty()) return;
  {
    std::lock_guard<std::mutex> lk(event_log_mu_);
    for (const GatewayEvent& e : events) {
      event_log_.push_back({next_event_seq_++, e});
      if (event_log_.size() > kEventLogRingSize) event_log_.pop_front();
      write_event_log_line(event_log_file_, e);
    }
  }

  // A critical, not-yet-cleared event is exactly what the flight recorder exists for: the crash
  // nobody had debugging on for. save_flight_recording() itself is not called here (that would
  // recurse into emit()); the save is inlined and its own event appended to `extra`.
  std::vector<GatewayEvent> extra;
  for (const GatewayEvent& e : events) {
    if (e.code == 0 || e.cleared || e.severity != static_cast<uint8_t>(ROVER_SEVERITY_CRITICAL)) continue;
    if (!cfg_.flight_recorder || cfg_.flight_recorder_dir.empty()) continue;
    char hex[8];
    std::snprintf(hex, sizeof hex, "%04x", e.code);
    const std::string path = flight_recorder_.save(cfg_.flight_recorder_dir, std::string("critical_") + hex);
    if (path.empty()) continue;
    GatewayEvent fe;
    fe.type = EventType::FlightRecorderSaved;
    fe.time_ns = e.time_ns;
    extra.push_back(fe);
    std::lock_guard<std::mutex> lk(event_log_mu_);
    event_log_.push_back({next_event_seq_++, fe});
    if (event_log_.size() > kEventLogRingSize) event_log_.pop_front();
    write_event_log_line(event_log_file_, fe);
  }

  std::vector<std::pair<int, EventCallback>> subs;
  {
    std::lock_guard<std::mutex> lk(sub_mu_);
    subs = subscribers_;
  }
  for (const auto& e : events)
    for (const auto& s : subs) s.second(e);
  for (const auto& e : extra)
    for (const auto& s : subs) s.second(e);
}

std::vector<EventRecord> Gateway::events_since(uint64_t seq, size_t max) const {
  std::lock_guard<std::mutex> lk(event_log_mu_);
  std::vector<EventRecord> out;
  if (seq == 0) {
    const size_t start = event_log_.size() > max ? event_log_.size() - max : 0;
    for (size_t i = start; i < event_log_.size(); ++i) out.push_back(event_log_[i]);
    return out;
  }
  for (const EventRecord& r : event_log_) {
    if (r.seq <= seq) continue;
    out.push_back(r);
    if (out.size() >= max) break;
  }
  return out;
}

std::string Gateway::save_flight_recording(const std::string& reason) {
  if (!cfg_.flight_recorder) return "";
  const std::string path = flight_recorder_.save(cfg_.flight_recorder_dir, reason);
  if (!path.empty()) {
    GatewayEvent fe;
    fe.type = EventType::FlightRecorderSaved;
    fe.time_ns = mono_ns();
    emit({fe});
  }
  return path;
}

bool Gateway::send(const CanFrame& f) {
  if (f.id == kInvalidFrameId) {
    tx_failures_.fetch_add(1, std::memory_order_relaxed);
    return false;
  }
  if (!bus_->send(f)) {
    tx_failures_.fetch_add(1, std::memory_order_relaxed);
    return false;
  }
  tx_frames_.fetch_add(1, std::memory_order_relaxed);
  return true;
}

void Gateway::apply_realtime(int priority_offset) {
#ifdef __linux__
  if (cfg_.rt_priority > 0) {
    sched_param sp{};
    sp.sched_priority = std::max(1, cfg_.rt_priority - priority_offset);
    if (pthread_setschedparam(pthread_self(), SCHED_FIFO, &sp) != 0 && !rt_warned_.exchange(true)) {
      log("warning: could not set SCHED_FIFO priority (needs root or CAP_SYS_NICE); cycle timing may jitter");
    }
  }
  if (cfg_.cpu_affinity >= 0) {
    cpu_set_t set;
    CPU_ZERO(&set);
    CPU_SET(cfg_.cpu_affinity, &set);
    if (pthread_setaffinity_np(pthread_self(), sizeof(set), &set) != 0)
      log("warning: could not pin CAN thread to core " + std::to_string(cfg_.cpu_affinity));
  }
#else
  (void)priority_offset;
#endif
}

// Called with mu_ held after anything that may have changed the election. Arming belongs to one
// (role, term): a claim or step-down invalidates it, the same way nodes disarm on a term change.
void Gateway::note_role_change_locked(Role old_role, uint16_t old_term) {
  if (election_.role() != old_role || (election_.is_master() && election_.term() != old_term)) armed_ = false;
}

// ---------------------------------------------------------------------------------------------
// RX path
// ---------------------------------------------------------------------------------------------

void Gateway::rx_loop() {
  apply_realtime(1);
  CanFrame f;
  while (running_.load()) {
    const RecvStatus st = bus_->recv(f, 10);
    if (st == RecvStatus::Closed) break;
    if (st == RecvStatus::Timeout) continue;
    handle_frame(f);
  }
}

void Gateway::handle_frame(const CanFrame& f) {
  rx_frames_.fetch_add(1, std::memory_order_relaxed);
  if (cfg_.flight_recorder) flight_recorder_.record(f);

  if (f.is_error) {
    error_frames_.fetch_add(1, std::memory_order_relaxed);
    if (f.err_class & kErrClassBusOff) bus_off_.store(true);
    if (f.err_class & kErrClassRestarted) bus_off_.store(false);
    std::vector<GatewayEvent> events;
    {
      std::lock_guard<std::mutex> lk(mu_);
      bus_errors_.on_error_frame(f, f.rx_ns, events);
    }
    emit(events);
    return;
  }
  Decoded d;
  if (!decode_frame(f, d)) {
    undecoded_.fetch_add(1, std::memory_order_relaxed);
    return;
  }

  std::vector<GatewayEvent> events;
  std::vector<CanFrame> to_send;
  {
    std::lock_guard<std::mutex> lk(mu_);
    const Role old_role = election_.role();
    const uint16_t old_term = election_.term();

    std::visit(
        [&](const auto& m) {
          using T = std::decay_t<decltype(m)>;
          if constexpr (std::is_same_v<T, HostHbMsg>) {
            election_.on_host_hb(m, d.rx_ns, events);
          } else if constexpr (std::is_same_v<T, LeadRequestMsg>) {
            election_.on_lead_request(m, d.rx_ns, events);
          } else if constexpr (std::is_same_v<T, EstopMsg>) {
            // Anyone may send E-stop. Any frame on the E-stop ID latches it, whatever its payload:
            // when in doubt, stop.
            estop_latched_ = true;
            armed_ = false;
            GatewayEvent e;
            e.type = EventType::EstopSeen;
            e.time_ns = d.rx_ns;
            events.push_back(e);
          } else if constexpr (std::is_same_v<T, EstopReleaseMsg>) {
            estop_latched_ = false;
            GatewayEvent e;
            e.type = EventType::EstopReleased;
            e.time_ns = d.rx_ns;
            e.node_id = m.master_node_id;
            events.push_back(e);
          } else if constexpr (std::is_same_v<T, NodeEventMsg>) {
            // docs: "Error reporting and diagnostics" > NODE_EVENT. Every event is forwarded as
            // one numbered entry of the gateway's own event stream, and a skipped sequence number
            // means a frame was lost: ask the node for its black box instead of guessing.
            GatewayEvent e;
            e.type = EventType::NodeEvent;
            e.time_ns = d.rx_ns;
            e.node_id = m.node_id;
            e.code = m.error_code;
            e.detail = m.detail;
            e.severity = m.severity;
            e.cleared = m.cleared != 0;
            events.push_back(e);

            auto it = last_event_seq_.find(m.node_id);
            if (it == last_event_seq_.end()) {
              last_event_seq_.emplace(m.node_id, m.event_seq);
            } else {
              const uint8_t expected = static_cast<uint8_t>(it->second + 1);
              if (m.event_seq != expected) {
                GatewayEvent g;
                g.type = EventType::EventGapDetected;
                g.time_ns = d.rx_ns;
                g.node_id = m.node_id;
                g.detail = static_cast<uint8_t>(m.event_seq - expected);
                events.push_back(g);

                ServiceMsg req;
                req.node_id = m.node_id;
                req.host = cfg_.host_id;
                req.opcode = kServiceOpLogRead;
                req.transaction_id = ++service_txn_counter_;
                req.param_index = 0;  // newest entry first
                req.value_raw = 0;
                to_send.push_back(make_service_req(req));
              }
              it->second = m.event_seq;
            }
          }
        },
        d.msg);
    registry_.on_decoded(d, events);
    note_role_change_locked(old_role, old_term);
  }
  for (const CanFrame& sf : to_send) send(sf);
  emit(events);
}

// ---------------------------------------------------------------------------------------------
// Control thread: election timers, node timeouts, HOST_HB, and the 20 ms cycle
// ---------------------------------------------------------------------------------------------

void Gateway::control_loop() {
  apply_realtime(0);
  int64_t next_ns = mono_ns();
  int64_t last_hb_ns = 0;
  int64_t stats_ns = next_ns;
  uint64_t stats_rx = rx_frames_.load();
  uint64_t tick = 0;

  while (running_.load()) {
    next_ns += kSchedulerTickNs;
    sleep_until_ns(next_ns);
    if (!running_.load()) break;
    const int64_t now = mono_ns();
    const int64_t lateness = now - next_ns;
    ++tick;
    if (lateness > kSchedulerTickNs) next_ns = now;  // fell a whole tick behind: resync, don't burst

    std::vector<GatewayEvent> events;
    std::vector<CanFrame> out;
    {
      std::lock_guard<std::mutex> lk(mu_);
      const Role old_role = election_.role();
      const uint16_t old_term = election_.term();
      registry_.tick(now, events);
      election_.tick(now, events);
      note_role_change_locked(old_role, old_term);

      if (now - stats_ns >= kNsPerSec) {
        const uint64_t rx = rx_frames_.load();
        rx_fps_ = static_cast<double>(rx - stats_rx) * kNsPerSec / static_cast<double>(now - stats_ns);
        stats_rx = rx;
        stats_ns = now;
      }

      // HOST_HB every 50 ms, and immediately after a claim or step-down (a claim must be
      // announced within 50 ms; waiting for the next slot could come close to that).
      const bool urgent = election_.take_hb_urgent();
      if (urgent || now - last_hb_ns >= kHostHbPeriodNs - kSchedulerTickNs / 2) {
        HostHbMsg hb;
        hb.host_node = cfg_.host_id;
        hb.role = static_cast<uint8_t>(election_.role());
        hb.tie_break = cfg_.effective_tie_break();
        hb.term = election_.term();
        hb.believed_master = election_.believed_master();
        hb.flags = static_cast<uint8_t>((estop_latched_ ? kHbFlagEstopLatched : 0) | (armed_ ? kHbFlagArmed : 0));
        hb.uptime_s = static_cast<uint16_t>(std::min<int64_t>((now - start_ns_) / kNsPerSec, 65535));
        out.push_back(make_host_hb(hb));
        last_hb_ns = now;
      }

      // The 20 ms cycle (every second tick): only the master transmits it.
      if (tick % 2 == 0 && election_.is_master()) {
        out.push_back(make_sync(cycle_counter_));
        for (uint8_t d = 1; d <= kNumDrives; ++d) {
          const TargetSlot& slot = targets_[d - 1];
          const bool stale = slot.set_ns == 0 || now - slot.set_ns > kTargetStaleNs;
          const bool zero = stale || estop_latched_;
          DriveCmdMsg c;
          c.drive = d;
          c.seq = cycle_counter_ & 0xF;
          c.sender_host = cfg_.host_id;
          c.term_low = election_.term() & 0x3;
          c.mode = static_cast<uint8_t>(zero ? DriveMode::Velocity : slot.target.mode);
          c.target_a = zero ? 0 : scale_target(slot.target.a);
          c.target_b = zero ? 0 : scale_target(slot.target.b);
          c.accel_limit = scale_accel(slot.target.accel);
          out.push_back(make_drive_cmd(c));
        }
        ++cycle_counter_;
        ++cycles_sent_;
        worst_lateness_ns_ = std::max(worst_lateness_ns_, lateness);
        if (lateness > kLateCycleNs) {
          ++late_cycles_;
          const int64_t last = last_late_log_ns_.load();
          if (now - last > kNsPerSec) {  // at most one log line per second
            last_late_log_ns_.store(now);
            log("late cycle: started " + std::to_string(lateness / 1000) + " us after its deadline");
          }
        }
      }

      // Debug-level auto-off: a timer that expired reverts to level 0 and, while master, that
      // revert goes on the bus like any other DEBUG_LEVEL change.
      debug_.tick(now, events, [&](uint8_t node, uint8_t level, uint8_t mask) {
        if (!election_.is_master()) return;
        DebugLevelMsg m;
        m.target_node = node;
        m.level = level;
        m.category_mask = mask;
        m.auto_off_timer_min = 0;
        m.master_node_id = cfg_.host_id;
        m.master_term = election_.term() & 0xFF;
        out.push_back(make_debug_level(m));
      });

      // The gateway link to the other host (docs: "The gateway link"). A standby host forwards
      // its local drive targets every cycle so the leader can apply them; the leader applies
      // whatever the standby last forwarded via on_link_targets(), not anything sent here.
      if (link_) {
        link_->tick(now, events);
        if (now - last_link_alive_ns_ >= kLinkAlivePeriodNs) {
          last_link_alive_ns_ = now;
          const uint16_t uptime_s = static_cast<uint16_t>(std::min<int64_t>((now - start_ns_) / kNsPerSec, 65535));
          link_->send_alive(cfg_.host_id, election_.role(), election_.term(), uptime_s);
        }
        if (tick % 2 == 0 && !election_.is_master()) {
          std::array<WheelTarget, kNumDrives> snapshot{};
          for (size_t i = 0; i < kNumDrives; ++i) snapshot[i] = targets_[i].target;
          link_->send_targets(cfg_.host_id, snapshot);
        }
      }
    }
    for (const CanFrame& f : out) send(f);
    emit(events);
  }
}

// ---------------------------------------------------------------------------------------------
// Local API
// ---------------------------------------------------------------------------------------------

void Gateway::set_targets(const std::array<WheelTarget, kNumDrives>& targets) {
  const int64_t now = mono_ns();
  std::lock_guard<std::mutex> lk(mu_);
  for (size_t i = 0; i < kNumDrives; ++i) targets_[i] = {targets[i], now};
}

CmdResult Gateway::set_wheel_target(uint8_t drive, const WheelTarget& target) {
  if (drive < 1 || drive > kNumDrives) return CmdResult::BadArgument;
  const int64_t now = mono_ns();
  std::lock_guard<std::mutex> lk(mu_);
  targets_[drive - 1] = {target, now};
  return CmdResult::Ok;
}

CmdResult Gateway::arm() {
  CanFrame f;
  {
    std::lock_guard<std::mutex> lk(mu_);
    if (!running_.load()) return CmdResult::NotRunning;
    if (!election_.is_master()) return CmdResult::NotMaster;
    if (estop_latched_) return CmdResult::EstopLatched;
    f = make_nmt(0, static_cast<uint8_t>(NmtCommand::Arm), cfg_.host_id, election_.term() & 0xFF);
    armed_ = true;
  }
  if (send(f)) return CmdResult::Ok;
  std::lock_guard<std::mutex> lk(mu_);
  armed_ = false;
  return CmdResult::SendFailed;
}

CmdResult Gateway::disarm() {
  CanFrame f;
  {
    std::lock_guard<std::mutex> lk(mu_);
    if (!running_.load()) return CmdResult::NotRunning;
    if (!election_.is_master()) return CmdResult::NotMaster;
    f = make_nmt(0, static_cast<uint8_t>(NmtCommand::Disarm), cfg_.host_id, election_.term() & 0xFF);
    armed_ = false;
  }
  return send(f) ? CmdResult::Ok : CmdResult::SendFailed;
}

CmdResult Gateway::estop() {
  GatewayEvent e;
  {
    std::lock_guard<std::mutex> lk(mu_);
    if (!running_.load()) return CmdResult::NotRunning;
    estop_latched_ = true;
    armed_ = false;
    e.type = EventType::EstopSeen;
    e.time_ns = mono_ns();
    e.node_id = cfg_.host_id;
  }
  const bool ok = send(make_estop());  // latched locally even if the bus refused the frame
  emit({e});
  return ok ? CmdResult::Ok : CmdResult::SendFailed;
}

CmdResult Gateway::release_estop() {
  CanFrame f;
  GatewayEvent e;
  {
    std::lock_guard<std::mutex> lk(mu_);
    if (!running_.load()) return CmdResult::NotRunning;
    if (!election_.is_master()) return CmdResult::NotMaster;
    f = make_estop_release(cfg_.host_id, election_.term() & 0xFF);
    estop_latched_ = false;
    e.type = EventType::EstopReleased;
    e.time_ns = mono_ns();
    e.node_id = cfg_.host_id;
  }
  const bool ok = send(f);
  emit({e});
  return ok ? CmdResult::Ok : CmdResult::SendFailed;
}

CmdResult Gateway::request_lead(uint8_t host_id) {
  if (host_id != kNodeOrin && host_id != kNodeRpi) return CmdResult::BadArgument;
  LeadRequestMsg req;
  req.sender_node = cfg_.host_id;
  req.requested_leader = host_id;
  std::vector<GatewayEvent> events;
  {
    std::lock_guard<std::mutex> lk(mu_);
    if (!running_.load()) return CmdResult::NotRunning;
    req.request_counter = ++lead_request_counter_;  // new value per call: a repeated click is a new request
    // Our own frames are not echoed back by the bus, so apply the request locally as well.
    const Role old_role = election_.role();
    const uint16_t old_term = election_.term();
    election_.on_lead_request(req, mono_ns(), events);
    note_role_change_locked(old_role, old_term);
  }
  const bool ok = send(make_lead_request(req));
  emit(events);
  return ok ? CmdResult::Ok : CmdResult::SendFailed;
}

double Gateway::estimate_bus_load_locked(uint8_t target_node, uint8_t level) const {
  // Fixed cost, independent of any node's debug level: HOST_HB from both hosts, SYNC and the
  // six DRIVE_CMDs every 20 ms cycle.
  double hz = 2 * 20.0 + 50.0 + kNumDrives * 50.0;
  for (const NodeInfo& n : registry_.snapshot(mono_ns())) {
    const bool is_target = target_node == 0 || target_node == n.node_id;
    const uint8_t lvl = is_target ? level : n.debug_level;
    if (n.node_id == kNodeFpga) {
      hz += 2 * 50.0 + 10.0;  // FPGA_SENSORS_A/_B at 50 Hz + heartbeat: fixed, no telemetry/debug pages
      continue;
    }
    if (n.node_id < kNodeFirstDrive || n.node_id >= kNodeFirstDrive + kNumDrives) continue;
    hz += 2 * 50.0 + 10.0;  // DRIVE_FB_A/_B at 50 Hz + heartbeat: fixed
    const uint16_t telem0_id = static_cast<uint16_t>((ROVER_CLASS_TELEMETRY << 8) | (n.node_id << 3) | 0);
    const uint16_t telem1_id = static_cast<uint16_t>((ROVER_CLASS_TELEMETRY << 8) | (n.node_id << 3) | 1);
    const uint16_t debug_a_id = static_cast<uint16_t>((ROVER_CLASS_TELEMETRY << 8) | (n.node_id << 3) | 2);
    const uint16_t debug_b_id = static_cast<uint16_t>((ROVER_CLASS_TELEMETRY << 8) | (n.node_id << 3) | 3);
    hz += NodeRegistry::expected_hz(telem0_id, lvl) + NodeRegistry::expected_hz(telem1_id, lvl) +
          NodeRegistry::expected_hz(debug_a_id, lvl) + NodeRegistry::expected_hz(debug_b_id, lvl);
  }
  return hz * kLinkBitsPerFrame / kCanBitrate;
}

CmdResult Gateway::set_debug_level(uint8_t target_node, uint8_t level, uint8_t category_mask,
                                    uint16_t auto_off_min) {
  std::vector<GatewayEvent> events;
  CanFrame f;
  f.id = kInvalidFrameId;
  DebugLevelMsg link_msg{};
  bool forward_over_link = false;
  CmdResult result = CmdResult::Ok;
  {
    std::lock_guard<std::mutex> lk(mu_);
    if (!running_.load()) return CmdResult::NotRunning;
    if (target_node > kMaxNodeId || level > 3) return CmdResult::BadArgument;
    const bool ok = debug_.request(target_node, level, category_mask, auto_off_min, mono_ns(), events);
    if (!ok) {
      result = CmdResult::Refused;
    } else if (election_.is_master()) {
      DebugLevelMsg m;
      m.target_node = target_node;
      m.level = level;
      m.category_mask = category_mask;
      m.auto_off_timer_min = auto_off_min;
      m.master_node_id = cfg_.host_id;
      m.master_term = election_.term() & 0xFF;
      f = make_debug_level(m);
    } else if (link_) {
      link_msg.target_node = target_node;
      link_msg.level = level;
      link_msg.category_mask = category_mask;
      link_msg.auto_off_timer_min = auto_off_min;
      forward_over_link = true;
    }
  }
  if (f.id != kInvalidFrameId) send(f);
  if (forward_over_link) link_->send_debug(cfg_.host_id, link_msg);
  emit(events);
  return result;
}

GatewayState Gateway::state() const {
  GatewayState s;
  const int64_t now = mono_ns();
  s.time_ns = now;
  s.host_id = cfg_.host_id;
  {
    std::lock_guard<std::mutex> lk(mu_);
    s.role = election_.role();
    s.term = election_.term();
    s.believed_master = election_.believed_master();
    s.leader_lost = election_.leader_lost();
    s.armed = armed_;
    s.estop_latched = estop_latched_;
    s.cycles_sent = cycles_sent_;
    s.late_cycles = late_cycles_;
    s.worst_lateness_ns = worst_lateness_ns_;
    for (uint8_t h = 1; h < s.peers.size(); ++h) s.peers[h] = election_.peer(h);
    s.bus.rx_fps = rx_fps_;
    s.nodes = registry_.snapshot(now);
    s.rates = registry_.rates();
    s.debug_level = debug_.level_of(0);
    s.debug_on = debug_.any_above_normal();
    s.debug_auto_off_ns = debug_.remaining_auto_off_ns(now);
    s.bus.error_types = bus_errors_.totals();
  }
  s.bus.rx_frames = rx_frames_.load();
  s.bus.tx_frames = tx_frames_.load();
  s.bus.tx_failures = tx_failures_.load();
  s.bus.error_frames = error_frames_.load();
  s.bus.undecoded_frames = undecoded_.load();
  s.bus.bus_off = bus_off_.load();
  s.flight_recorder_enabled = cfg_.flight_recorder;
  s.link_configured = link_ != nullptr;
  s.link_up = link_ != nullptr && link_->peer_up(now);
  return s;
}

}  // namespace anv::gw
