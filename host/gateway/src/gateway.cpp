#include "anv_gateway/gateway.hpp"

#include <algorithm>
#include <cerrno>
#include <cmath>
#include <ctime>
#include <type_traits>

#include "anv_gateway/encoder.hpp"

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
  }
  return "?";
}

Gateway::Gateway(const GatewayConfig& cfg, std::unique_ptr<ICanBus> bus)
    : cfg_(cfg), bus_(std::move(bus)), election_(election_config(cfg)), allocator_(cfg.node_id_table) {}

Gateway::~Gateway() { stop(); }

bool Gateway::start() {
  bool expected = false;
  if (!running_.compare_exchange_strong(expected, true)) return false;
  start_ns_ = mono_ns();
  std::string table_error;
  {
    std::lock_guard<std::mutex> lk(mu_);
    election_.start(start_ns_);  // standby: a booting host never claims the lead by itself
    if (!allocator_.load(&table_error)) allocator_ = IdAllocator(cfg_.node_id_table);  // start empty
  }
  if (!table_error.empty()) log("warning: node ID table not loaded (" + table_error + "); starting empty");
  rx_thread_ = std::thread([this] { rx_loop(); });
  control_thread_ = std::thread([this] { control_loop(); });
  return true;
}

void Gateway::stop() {
  if (!running_.exchange(false)) return;
  bus_->close();
  if (rx_thread_.joinable()) rx_thread_.join();
  if (control_thread_.joinable()) control_thread_.join();
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
    std::lock_guard<std::mutex> lk(mu_);
    recent_events_.insert(recent_events_.end(), events.begin(), events.end());
    if (recent_events_.size() > kRecentEvents)
      recent_events_.erase(recent_events_.begin(), recent_events_.end() - kRecentEvents);
  }
  std::vector<std::pair<int, EventCallback>> subs;
  {
    std::lock_guard<std::mutex> lk(sub_mu_);
    subs = subscribers_;
  }
  for (const auto& e : events)
    for (const auto& s : subs) s.second(e);
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

// The master hands out node IDs. With no live master (a lone RPi on the bench, or before anyone
// chose a leader), the live host with the lowest tie-break does, so boards get addresses without
// an operator. Only one host ever answers, keeping one sender per assignment.
bool Gateway::is_id_allocator_locked(int64_t now_ns) const {
  if (!cfg_.allocate_ids) return false;
  if (election_.is_master()) return true;
  for (uint8_t h = 1; h <= kNodeTool; ++h) {
    if (h == cfg_.host_id) continue;
    const PeerHost& p = election_.peer(h);
    if (!p.seen || now_ns - p.last_seen_ns > kLeaderLostNs) continue;
    if (p.role == static_cast<uint8_t>(Role::Master)) return false;
    if (p.tie_break < cfg_.effective_tie_break()) return false;
  }
  return true;
}

void Gateway::log_text_locked(TextLogEntry e) {
  texts_.push_back(std::move(e));
  if (texts_.size() > kTextLogSize) texts_.erase(texts_.begin(), texts_.end() - kTextLogSize);
}

void Gateway::on_id_request_locked(const NodeIdRequestMsg& m, int64_t now_ns, std::vector<GatewayEvent>& events,
                                   std::vector<CanFrame>& out) {
  if (!is_id_allocator_locked(now_ns)) return;
  // Skip IDs a board is already using: a board that kept an old ID from another table must not
  // end up sharing it with a newcomer.
  const auto nodes = registry_.snapshot(now_ns);
  auto in_use = [&](uint8_t id) {
    for (const NodeInfo& n : nodes)
      if (n.node_id == id && n.seen) return true;
    return false;
  };
  std::string save_error;
  const uint8_t id = allocator_.assign(m.mac, m.hw_target, in_use, &save_error);
  if (!save_error.empty()) log("warning: node ID table not saved: " + save_error);
  if (id == 0) {
    log("node ID pool exhausted; board " + IdAllocator::mac_to_string(m.mac) + " left unconfigured");
    return;
  }
  NodeIdAssignMsg a;
  a.host = cfg_.host_id;
  a.mac = m.mac;
  a.assigned_id = id;
  out.push_back(make_node_id_assign(a));
  GatewayEvent e;
  e.type = EventType::NodeIdAssigned;
  e.time_ns = now_ns;
  e.node_id = id;
  e.mac = m.mac;
  events.push_back(e);
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
  if (f.is_error) {
    error_frames_.fetch_add(1, std::memory_order_relaxed);
    if (f.err_class & kErrClassBusOff) bus_off_.store(true);
    if (f.err_class & kErrClassRestarted) bus_off_.store(false);
    return;
  }
  Decoded d;
  if (!decode_frame(f, d)) {
    undecoded_.fetch_add(1, std::memory_order_relaxed);
    return;
  }

  std::vector<GatewayEvent> events;
  std::vector<CanFrame> out;
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
          } else if constexpr (std::is_same_v<T, NodeIdRequestMsg>) {
            on_id_request_locked(m, d.rx_ns, events, out);
          } else if constexpr (std::is_same_v<T, TextFromNodeMsg>) {
            log_text_locked({d.rx_ns, false, m.node, m.seq, m.text});
          }
        },
        d.msg);
    registry_.on_decoded(d, events);
    note_role_change_locked(old_role, old_term);
  }
  for (const CanFrame& f : out) send(f);
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

CmdResult Gateway::forget_node(uint8_t node_id) {
  std::string save_error;
  bool found;
  {
    std::lock_guard<std::mutex> lk(mu_);
    found = allocator_.forget_node(node_id, &save_error);
  }
  if (!save_error.empty()) log("warning: node ID table not saved: " + save_error);
  return found ? CmdResult::Ok : CmdResult::BadArgument;
}

CmdResult Gateway::send_text(uint8_t node, const std::string& text, uint8_t* seq_out) {
  CanFrame f;
  uint8_t seq;
  {
    std::lock_guard<std::mutex> lk(mu_);
    if (!running_.load()) return CmdResult::NotRunning;
    seq = static_cast<uint8_t>(text_seq_ % 255 + 1);  // 1-255: 0 means "the node's own text"
    f = make_text_to_node(cfg_.host_id, node, seq, text);
    if (f.id == kInvalidFrameId) return CmdResult::BadArgument;
    text_seq_ = seq;
    log_text_locked({mono_ns(), true, node, seq, text});
  }
  if (seq_out) *seq_out = seq;
  return send(f) ? CmdResult::Ok : CmdResult::SendFailed;
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
    for (NodeInfo& n : s.nodes) {
      if (const IdAssignment* a = allocator_.find_by_node(n.node_id)) {
        n.mac = a->mac;
        n.hw_target = a->hw_target;
      }
    }
    s.id_allocator = is_id_allocator_locked(now);
    s.id_table = allocator_.table();
    s.recent_events = recent_events_;
    s.texts = texts_;
  }
  s.bus.rx_frames = rx_frames_.load();
  s.bus.tx_frames = tx_frames_.load();
  s.bus.tx_failures = tx_failures_.load();
  s.bus.error_frames = error_frames_.load();
  s.bus.undecoded_frames = undecoded_.load();
  s.bus.bus_off = bus_off_.load();
  return s;
}

}  // namespace anv::gw
