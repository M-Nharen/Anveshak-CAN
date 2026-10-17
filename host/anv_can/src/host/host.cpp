#include "anv/host/host.hpp"

#include <cctype>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <limits>

#include "anv/isotp.hpp"  // monotonic_us

namespace anv::host {

namespace {

constexpr int64_t kMs = 1'000'000;

int64_t now_ns() { return monotonic_us() * 1000; }

int64_t period_ns(double hz) { return hz > 0 ? static_cast<int64_t>(1e9 / hz) : 0; }

// Raw (wire integer) helpers: the host decodes protocol frames raw and looks labels up itself.
int64_t num(const Values& v, const char* name, size_t i = 0) {
  auto it = v.find(name);
  if (it == v.end() || it->second.size() <= i) return 0;
  return static_cast<int64_t>(as_double(it->second[i]));
}
Scalar u(uint64_t x) { return Scalar{x}; }

const DecodeOptions kRaw = [] {
  DecodeOptions o;
  o.raw = true;
  o.labels = false;
  return o;
}();

const EncodeOptions kRawEnc = [] {
  EncodeOptions o;
  o.raw = true;
  return o;
}();

const Message* need(const Schema& s, const char* name) { return s.protocol.message(std::string(name)); }

bool parse_index(const std::string& text, uint16_t& out) {
  if (text.empty() || !std::isdigit(static_cast<unsigned char>(text[0]))) return false;
  char* end = nullptr;
  const unsigned long v = std::strtoul(text.c_str(), &end, 0);
  if (*end != '\0' || v > 0xFFFF) return false;
  out = static_cast<uint16_t>(v);
  return true;
}

}  // namespace

const char* to_string(Status s) {
  switch (s) {
    case Status::Ok: return "ok";
    case Status::BadIndex: return "bad_index";
    case Status::OutOfRange: return "out_of_range";
    case Status::NotWhileArmed: return "not_while_armed";
    case Status::StorageError: return "storage_error";
    case Status::ReadOnly: return "read_only";
    case Status::BadOp: return "bad_op";
    case Status::Busy: return "busy";
    case Status::Failed: return "failed";
    case Status::Timeout: return "timeout";
    case Status::NoSuchNode: return "no_such_node";
    case Status::UnknownName: return "unknown_name";
    case Status::BadValue: return "bad_value";
    case Status::NotLeader: return "not_leader";
    case Status::SendFailed: return "send_failed";
    case Status::NotRunning: return "not_running";
    case Status::WouldDeadlock: return "would_deadlock";
  }
  return "?";
}

bool value_to_wire(const ValueType& t, double value, uint32_t& wire, std::string* error) {
  auto fail = [&](const char* why) {
    if (error) *error = why;
    return false;
  };
  if (!std::isfinite(value)) return fail("not a finite number");
  if (t.is_float) {
    const float f = static_cast<float>(value);
    std::memcpy(&wire, &f, sizeof wire);
    return true;
  }
  const double r = std::round(value);
  if (t.is_bool) {
    if (r != 0 && r != 1) return fail("a bool is 0 or 1");
    wire = static_cast<uint32_t>(r);
    return true;
  }
  const int bits = t.bits > 32 ? 32 : t.bits;
  if (t.is_signed) {
    const double lo = -std::ldexp(1.0, bits - 1), hi = std::ldexp(1.0, bits - 1) - 1;
    if (r < lo || r > hi) return fail("doesn't fit the parameter's type");
    wire = static_cast<uint32_t>(static_cast<int32_t>(r));  // sign-extended, like anv_value_t.i
  } else {
    if (r < 0 || r > std::ldexp(1.0, bits) - 1) return fail("doesn't fit the parameter's type");
    wire = static_cast<uint32_t>(r);
  }
  return true;
}

double value_from_wire(const ValueType& t, uint32_t wire) {
  if (t.is_float) {
    float f;
    std::memcpy(&f, &wire, sizeof f);
    return f;
  }
  if (t.is_bool) return wire != 0 ? 1 : 0;
  if (t.is_signed) {
    // Sign-extend from the type's width in case a node sent only the low bits.
    const int bits = t.bits > 32 ? 32 : t.bits;
    const int64_t mask = (int64_t{1} << bits) - 1, sign = int64_t{1} << (bits - 1);
    const int64_t v = static_cast<int64_t>(wire) & mask;
    return static_cast<double>((v ^ sign) - sign);
  }
  const int bits = t.bits > 32 ? 32 : t.bits;
  return static_cast<double>(bits == 32 ? wire : wire & ((1u << bits) - 1));
}

// ---------------------------------------------------------------------------------------

Host::Host(Link& link, std::shared_ptr<const Schema> schema, HostConfig config)
    : link_(link),
      schema_(std::move(schema)),
      cfg_(config),
      m_estop_(need(*schema_, "estop")),
      m_estop_release_(need(*schema_, "estop_release")),
      m_sync_(need(*schema_, "sync")),
      m_nmt_(need(*schema_, "nmt")),
      m_host_hb_(need(*schema_, "host_heartbeat")),
      m_lead_req_(need(*schema_, "lead_request")),
      m_hb_(need(*schema_, "heartbeat")),
      m_id_req_(need(*schema_, "id_request")),
      m_id_assign_(need(*schema_, "id_assign")),
      m_service_(need(*schema_, "service")),
      m_event_(need(*schema_, "event")),
      election_([&] {
        ElectionConfig e;
        e.host_id = config.host_id;
        e.tie_break = config.tie_break != 0 ? config.tie_break : config.host_id;
        e.observer = config.observer;
        e.auto_takeover = config.auto_takeover;
        e.leader_timeout_ns = config.leader_timeout_ms * kMs;
        return e;
      }()),
      registry_(*schema_, RegistryConfig{config.node_timeout_ms * kMs}),
      allocator_(static_cast<uint8_t>(config.id_first ? config.id_first : schema_->node_ids.first),
                 static_cast<uint8_t>(config.id_last ? config.id_last : schema_->node_ids.last),
                 config.id_table_path) {}

Host::~Host() { stop(); }

bool Host::set_callbacks(HostCallbacks callbacks) {
  if (running_) return false;
  cb_ = std::move(callbacks);
  return true;
}

bool Host::start(std::string* error) {
  if (running_) return true;
  for (const Message* m : {m_estop_, m_estop_release_, m_sync_, m_nmt_, m_host_hb_, m_lead_req_, m_hb_, m_id_req_,
                           m_id_assign_, m_service_, m_event_}) {
    // A schema without these isn't the v2 protocol; nothing sensible can run on it.
    if (m == nullptr) {
      if (error) *error = "the schema's protocol is missing a message anv_host needs";
      return false;
    }
  }
  const uint32_t id = cfg_.host_id;
  if (id < schema_->node_ids.first || id > schema_->node_ids.last) {
    if (error) *error = "host_id must be a node ID (" + std::to_string(schema_->node_ids.first) + ".." +
                        std::to_string(schema_->node_ids.last) + ")";
    return false;
  }
  {
    std::lock_guard<std::mutex> lk(mu_);
    if (!allocator_.load(error)) return false;
    for (const Assignment& a : allocator_.table()) registry_.set_mac(a.node_id, a.mac);
    election_.start();
    start_ns_ = now_ns();
    next_hb_ns_ = start_ns_;
    running_ = true;
    // Under the lock, so the thread can't run a callback before thread_id_ is set.
    thread_ = std::thread([this] { run(); });
    thread_id_ = thread_.get_id();
  }
  return true;
}

void Host::stop() {
  if (!running_.exchange(false)) return;
  if (thread_.joinable()) thread_.join();
  std::lock_guard<std::mutex> lk(mu_);
  commands_.clear();
  cv_.notify_all();  // wake blocked clients: they see !running_
}

void Host::run() {
  while (running_) {
    Frame f;
    const RecvStatus st = link_.recv(f, cfg_.tick_ms);
    if (st == RecvStatus::Closed) break;
    Notify notify;
    {
      std::lock_guard<std::mutex> lk(mu_);
      const int64_t now = now_ns();
      if (st == RecvStatus::Ok) handle(f, now, notify);
      periodic(now, notify);
    }
    cv_.notify_all();
    for (auto& fn : notify) fn();
  }
  running_ = false;
  cv_.notify_all();
}

// --- receiving ---------------------------------------------------------------------------

void Host::handle(const Frame& f, int64_t now, Notify& out) {
  if (!f.extended || f.is_error) return;
  const IdParts id = schema_->id_layout.parse(f.id);
  const uint8_t src = static_cast<uint8_t>(id.src), dst = static_cast<uint8_t>(id.dst);
  if (src == cfg_.host_id && id.port != m_estop_->port) return;  // our own ID: not for us

  if (id.port > schema_->protocol_last_port) {
    // A node's own message. Only decode it if the node's type matches our schema.
    const NodeType* t = usable_type_locked(src);
    if (t == nullptr || !cb_.telemetry) return;
    const DecodedFrame d = decode_frame(*schema_, f, t);
    if (d.message == nullptr || !d.error.empty() || d.message->kind == "command" || d.message->kind == "frame") return;
    const Message* m = d.message;
    out.push_back([this, src, m, v = d.values, ts = f.rx_ns] { cb_.telemetry(src, *m, v, ts); });
    return;
  }

  const Message* m = schema_->protocol.message(static_cast<uint16_t>(id.port));
  if (m == nullptr || m->kind == "frame") return;
  Values v;
  std::string err;
  if (!decode(*m, f.data.data(), f.len, v, &err, kRaw)) return;

  std::vector<ElectionEvent> ev;
  if (m == m_host_hb_) {
    HostHeartbeat hb;
    hb.src = src;
    const std::string role = enum_label("host_role", num(v, "role"));
    hb.role = role == "leader" ? Role::Leader : role == "observer" ? Role::Observer
              : role == "booting" ? Role::Booting : Role::Standby;
    hb.tie_break = static_cast<uint8_t>(num(v, "tie_break"));
    hb.term = static_cast<uint16_t>(num(v, "term"));
    hb.leader = static_cast<uint8_t>(num(v, "leader"));
    election_.on_host_heartbeat(hb, now, ev);
  } else if (m == m_lead_req_) {
    election_.on_lead_request(src, static_cast<uint8_t>(num(v, "leader")), static_cast<uint8_t>(num(v, "counter")),
                              now, ev);
  } else if (m == m_hb_) {
    NodeHeartbeat hb;
    hb.src = src;
    hb.state = static_cast<uint8_t>(num(v, "state"));
    hb.can_state = static_cast<uint8_t>(num(v, "can_state"));
    hb.debug_level = static_cast<uint8_t>(num(v, "debug_level"));
    hb.unsaved_params = num(v, "unsaved_params") != 0;
    hb.leader_seen = num(v, "leader_seen") != 0;
    hb.fault_active = num(v, "fault_active") != 0;
    hb.type_id = static_cast<uint16_t>(num(v, "type_id"));
    hb.schema_hash = static_cast<uint16_t>(num(v, "schema_hash"));
    hb.uptime_s = static_cast<uint16_t>(num(v, "uptime_s"));
    std::vector<RegistryEvent> rev;
    registry_.on_heartbeat(hb, now, rev);
    on_registry(rev, out);
  } else if (m == m_id_req_) {
    if (src == schema_->node_ids.unassigned) handle_id_request(src, v, out);
  } else if (m == m_service_) {
    handle_service(src, dst, v, out);
  } else if (m == m_estop_) {
    estop_latched_ = true;
    armed_ = false;
    commands_.clear();
    if (cb_.estop) out.push_back([this, src, r = static_cast<uint8_t>(num(v, "reason"))] { cb_.estop(src, r); });
  } else if (m == m_estop_release_) {
    estop_latched_ = false;
  } else if (m == m_event_) {
    if (!cb_.event) return;
    NodeEvent e;
    e.node = src;
    e.code = static_cast<uint16_t>(num(v, "code"));
    e.severity = enum_label("severity", num(v, "severity"));
    e.cleared = num(v, "cleared") != 0;
    e.detail = static_cast<uint16_t>(num(v, "detail"));
    e.uptime_s = static_cast<uint16_t>(num(v, "uptime_s"));
    const NodeType* t = usable_type_locked(src);
    const Event* known = t != nullptr ? t->event(e.code) : nullptr;
    if (known == nullptr) known = schema_->protocol.event(e.code);
    if (known != nullptr) e.name = known->name;
    out.push_back([this, e] { cb_.event(e); });
  }
  on_election(ev, now, out);
}

void Host::handle_id_request(uint8_t /*src*/, const Values& v, Notify& out) {
  if (!should_allocate()) return;
  uint8_t mac_bytes[6];
  for (size_t i = 0; i < 6; ++i) mac_bytes[i] = static_cast<uint8_t>(num(v, "mac", i));
  const uint64_t mac = IdAllocator::mac_from_bytes(mac_bytes);
  const uint16_t type_id = static_cast<uint16_t>(num(v, "type_id"));
  const auto in_use = [&](uint8_t id) {
    if (id == cfg_.host_id) return true;
    auto peers = election_.peers().find(id);
    if (peers != election_.peers().end() && peers->second.online) return true;
    if (!registry_.online(id)) return false;
    // A board that lost its ID (forget_id, new flash) may ask again while still listed under it.
    const Assignment* a = allocator_.find_by_node(id);
    return a == nullptr || a->mac != mac;
  };
  std::string save_error;
  const uint8_t id = allocator_.assign(mac, type_id, in_use, &save_error);
  if (id == 0) return;  // every ID in use: say nothing, the board keeps asking
  registry_.set_mac(id, mac);
  Values a;
  FieldValue mac_field;
  for (uint8_t b : mac_bytes) mac_field.push_back(u(b));
  a["mac"] = mac_field;
  a["node_id"] = one(u(id));
  send_msg(*m_id_assign_, a, schema_->node_ids.unassigned);
  if (cb_.id_assigned) out.push_back([this, id, mac, type_id] { cb_.id_assigned(id, mac, type_id); });
}

bool Host::should_allocate() const {
  switch (cfg_.allocator) {
    case HostConfig::Allocator::Off: return false;
    case HostConfig::Allocator::Always: return true;
    case HostConfig::Allocator::Auto: break;
  }
  if (election_.leading()) return true;
  if (election_.leader() != 0 || cfg_.observer) return false;
  // Nobody leads yet (bring-up): the lowest-numbered host that could lead answers, so two
  // hosts never hand out IDs from two different tables.
  for (const auto& kv : election_.peers()) {
    const PeerHost& p = kv.second;
    if (p.online && p.role != Role::Observer && p.id < cfg_.host_id) return false;
  }
  return true;
}

void Host::handle_service(uint8_t src, uint8_t dst, const Values& v, Notify& out) {
  const uint32_t op = static_cast<uint32_t>(num(v, "op"));
  const uint8_t txn = static_cast<uint8_t>(num(v, "txn"));
  const uint32_t status = static_cast<uint32_t>(num(v, "status"));
  const uint32_t value = static_cast<uint32_t>(num(v, "value"));
  const bool from_node = registry_.find(src) != nullptr;
  if (!from_node) return;  // requests between hosts aren't ours to answer
  if (dst == cfg_.host_id) {
    auto it = pending_.find({src, txn});
    if (it != pending_.end() && it->second->op == op && !it->second->done) {
      it->second->done = true;
      it->second->status = status;
      it->second->value = value;
    }
  }
  // Watch every confirmed SET, whoever asked, so all hosts see setting changes.
  if (cb_.setting_changed && op == static_cast<uint32_t>(enum_value("service_op", "set")) &&
      status == static_cast<uint32_t>(enum_value("service_status", "ok"))) {
    const NodeType* t = usable_type_locked(src);
    const uint16_t index = static_cast<uint16_t>(num(v, "index"));
    const Param* p = t != nullptr ? t->param(index) : nullptr;
    if (p == nullptr) p = schema_->protocol.param(index);
    if (p != nullptr) {
      out.push_back([this, src, p, val = value_from_wire(p->type, value)] { cb_.setting_changed(src, *p, val); });
    }
  }
}

void Host::on_registry(const std::vector<RegistryEvent>& ev, Notify& out) {
  for (const RegistryEvent& e : ev) {
    const NodeInfo* n = registry_.find(e.node);
    if (n == nullptr) continue;
    const NodeInfo copy = *n;
    switch (e.kind) {
      case RegistryEvent::Kind::Appeared:
        if (cb_.node_appeared) out.push_back([this, copy, r = e.returned] { cb_.node_appeared(copy, r); });
        break;
      case RegistryEvent::Kind::Lost:
        if (cb_.node_lost) out.push_back([this, copy] { cb_.node_lost(copy); });
        break;
      case RegistryEvent::Kind::Rebooted:
        if (cb_.node_rebooted) out.push_back([this, copy] { cb_.node_rebooted(copy); });
        break;
      case RegistryEvent::Kind::StateChanged:
        if (cb_.node_state) out.push_back([this, copy, o = e.old_state] { cb_.node_state(copy, o); });
        break;
    }
  }
}

void Host::on_election(const std::vector<ElectionEvent>& ev, int64_t now, Notify& out) {
  for (const ElectionEvent& e : ev) {
    switch (e.kind) {
      case ElectionEvent::Kind::RoleChanged:
        if (e.role != Role::Leader) drop_commands();
        if (e.role == Role::Leader) next_sync_ns_ = now;
        if (cb_.role_changed) out.push_back([this, e] { cb_.role_changed(e.role, e.term); });
        break;
      case ElectionEvent::Kind::LeaderChanged:
        if (cb_.leader_changed) out.push_back([this, e] { cb_.leader_changed(e.host, e.term); });
        break;
      case ElectionEvent::Kind::LeaderLost:
        if (cb_.leader_lost) out.push_back([this, e] { cb_.leader_lost(e.host); });
        break;
      case ElectionEvent::Kind::TwoLeaders:
        if (cb_.two_leaders) out.push_back([this, e] { cb_.two_leaders(e.host, e.other); });
        break;
    }
  }
}

// --- sending -----------------------------------------------------------------------------

void Host::periodic(int64_t now, Notify& out) {
  std::vector<ElectionEvent> ev;
  election_.tick(now, ev);
  std::vector<RegistryEvent> rev;
  registry_.tick(now, rev);
  on_registry(rev, out);
  on_election(ev, now, out);

  // A claim or step-down is announced at once, not at the next slot.
  if (election_.take_urgent() || now >= next_hb_ns_) {
    send_host_heartbeat(now);
    next_hb_ns_ = now + period_ns(cfg_.heartbeat_hz);
  }
  if (!election_.leading()) return;
  if (cfg_.sync_hz > 0 && now >= next_sync_ns_) {
    Values v;
    v["counter"] = one(u(sync_counter_++));
    v["period_ms"] = one(u(static_cast<uint64_t>(std::lround(std::min(255.0, 1000.0 / cfg_.sync_hz)))));
    send_msg(*m_sync_, v, schema_->node_ids.broadcast);
    // Fixed steps, not now + period, so the cycle doesn't drift with scheduling jitter.
    next_sync_ns_ += period_ns(cfg_.sync_hz);
    if (next_sync_ns_ < now) next_sync_ns_ = now + period_ns(cfg_.sync_hz);
  }
  for (auto& kv : commands_) {
    Refresh& r = kv.second;
    if (now < r.next_ns) continue;
    link_.send(r.frame);
    r.next_ns += r.period_ns;
    if (r.next_ns < now) r.next_ns = now + r.period_ns;
  }
}

void Host::send_host_heartbeat(int64_t now) {
  Values v;
  v["role"] = one(u(static_cast<uint64_t>(enum_value("host_role", to_string(election_.role())))));
  v["tie_break"] = one(u(election_.config().tie_break));
  v["term"] = one(u(election_.term()));
  v["leader"] = one(u(election_.leader()));
  v["estop_latched"] = one(u(estop_latched_));
  v["armed"] = one(u(armed_));
  v["operator_link"] = one(u(operator_link_));
  v["autonomy_ready"] = one(u(autonomy_ready_));
  v["uptime_s"] = one(u(static_cast<uint64_t>((now - start_ns_) / 1'000'000'000) & 0xFFFF));
  send_msg(*m_host_hb_, v, schema_->node_ids.broadcast);
}

bool Host::send_msg(const Message& m, const Values& v, uint32_t dst, std::string* error) {
  Frame f;
  std::string err;
  if (!encode_frame(*schema_, m, v, cfg_.host_id, dst, f, &err, kRawEnc)) {
    if (error) *error = err;
    return false;
  }
  if (!link_.send(f)) {
    if (error) *error = "link refused the frame";
    return false;
  }
  return true;
}

void Host::drop_commands() {
  commands_.clear();
  armed_ = false;
}

// --- schema lookups ----------------------------------------------------------------------

int64_t Host::enum_value(const char* enum_name, const std::string& label) const {
  for (const auto& e : schema_->protocol.enums) {
    if (e.first != enum_name) continue;
    for (const auto& kv : e.second) {
      if (kv.first == label) return kv.second;
    }
  }
  return -1;
}

std::string Host::enum_label(const char* enum_name, int64_t value) const {
  for (const auto& e : schema_->protocol.enums) {
    if (e.first != enum_name) continue;
    for (const auto& kv : e.second) {
      if (kv.second == value) return kv.first;
    }
  }
  return std::to_string(value);
}

const NodeType* Host::usable_type_locked(uint8_t node) const {
  const NodeInfo* n = registry_.find(node);
  return n != nullptr ? n->usable_type() : nullptr;
}

// --- state queries -----------------------------------------------------------------------

std::vector<NodeInfo> Host::nodes() const {
  std::lock_guard<std::mutex> lk(mu_);
  std::vector<NodeInfo> out;
  for (const auto& kv : registry_.nodes()) out.push_back(kv.second);
  return out;
}

std::optional<NodeInfo> Host::node(uint8_t id) const {
  std::lock_guard<std::mutex> lk(mu_);
  const NodeInfo* n = registry_.find(id);
  if (n == nullptr) return std::nullopt;
  return *n;
}

std::vector<PeerHost> Host::hosts() const {
  std::lock_guard<std::mutex> lk(mu_);
  std::vector<PeerHost> out;
  for (const auto& kv : election_.peers()) out.push_back(kv.second);
  return out;
}

std::vector<Assignment> Host::id_table() const {
  std::lock_guard<std::mutex> lk(mu_);
  return allocator_.table();
}

bool Host::wait_for_node(uint8_t id, int timeout_ms) {
  std::unique_lock<std::mutex> lk(mu_);
  return cv_.wait_for(lk, std::chrono::milliseconds(timeout_ms), [&] { return !running_ || registry_.online(id); }) &&
         registry_.online(id);
}

bool Host::wait_for_lead(int timeout_ms) {
  std::unique_lock<std::mutex> lk(mu_);
  return cv_.wait_for(lk, std::chrono::milliseconds(timeout_ms), [&] { return !running_ || election_.leading(); }) &&
         election_.leading();
}

Role Host::role() const {
  std::lock_guard<std::mutex> lk(mu_);
  return election_.role();
}

uint16_t Host::term() const {
  std::lock_guard<std::mutex> lk(mu_);
  return election_.term();
}

uint8_t Host::leader() const {
  std::lock_guard<std::mutex> lk(mu_);
  return election_.leader();
}

bool Host::estop_latched() const {
  std::lock_guard<std::mutex> lk(mu_);
  return estop_latched_;
}

// --- leadership --------------------------------------------------------------------------

Status Host::request_lead(uint8_t host_id) {
  if (!running_) return Status::NotRunning;
  const uint8_t who = host_id != 0 ? host_id : cfg_.host_id;
  Notify notify;
  Status st = Status::Ok;
  {
    std::lock_guard<std::mutex> lk(mu_);
    const uint8_t counter = ++lead_counter_;
    Values v;
    v["leader"] = one(u(who));
    v["counter"] = one(u(counter));
    if (!send_msg(*m_lead_req_, v, schema_->node_ids.broadcast)) st = Status::SendFailed;
    std::vector<ElectionEvent> ev;
    const int64_t now = now_ns();
    election_.on_lead_request(cfg_.host_id, who, counter, now, ev);
    on_election(ev, now, notify);
  }
  cv_.notify_all();
  for (auto& fn : notify) fn();
  return st;
}

void Host::resign() {
  Notify notify;
  {
    std::lock_guard<std::mutex> lk(mu_);
    std::vector<ElectionEvent> ev;
    const int64_t now = now_ns();
    election_.resign(now, ev);
    on_election(ev, now, notify);
  }
  for (auto& fn : notify) fn();
}

void Host::set_operator_link(bool up) {
  std::lock_guard<std::mutex> lk(mu_);
  operator_link_ = up;
}

void Host::set_autonomy_ready(bool ready) {
  std::lock_guard<std::mutex> lk(mu_);
  autonomy_ready_ = ready;
}

bool Host::pin_id(uint64_t mac, uint8_t node_id, uint16_t type_id, std::string* error) {
  std::lock_guard<std::mutex> lk(mu_);
  if (!allocator_.pin(mac, node_id, type_id, error)) return false;
  registry_.set_mac(node_id, mac);
  return true;
}

// --- service clients ---------------------------------------------------------------------

Status Host::check_client(uint8_t node, const NodeType** type, std::string* error) const {
  if (!running_) return Status::NotRunning;
  std::lock_guard<std::mutex> lk(mu_);
  if (std::this_thread::get_id() == thread_id_) return Status::WouldDeadlock;
  const NodeInfo* n = registry_.find(node);
  if (n == nullptr || !n->online) {
    if (error) *error = "node " + std::to_string(node) + " is not online";
    return Status::NoSuchNode;
  }
  if (n->usable_type() == nullptr) {
    if (error) *error = "node " + std::to_string(node) + ": " + to_string(n->compat) + " (type_id " +
                        std::to_string(n->type_id) + ")";
    return Status::NoSuchNode;
  }
  *type = n->usable_type();
  return Status::Ok;
}

const Param* Host::find_param(uint8_t node, const std::string& name_or_index) const {
  const NodeType* t = nullptr;
  {
    std::lock_guard<std::mutex> lk(mu_);
    t = usable_type_locked(node);
  }
  uint16_t index = 0;
  const bool by_index = parse_index(name_or_index, index);
  for (const NodeType* nt : {t, &schema_->protocol}) {
    if (nt == nullptr) continue;
    const Param* p = by_index ? nt->param(index) : nt->param(name_or_index);
    if (p != nullptr) return p;
  }
  return nullptr;
}

Result Host::request(uint8_t node, const char* op_name, uint16_t index, uint32_t value, int retries) {
  Result r;
  const int64_t op = enum_value("service_op", op_name);
  if (op < 0) {
    r.status = Status::BadOp;
    r.error = std::string("schema has no service op ") + op_name;
    return r;
  }
  Pending p;
  p.node = node;
  p.op = static_cast<uint32_t>(op);
  std::unique_lock<std::mutex> lk(mu_);
  for (int attempt = 0; attempt <= retries; ++attempt) {
    if (!running_) {
      r.status = Status::NotRunning;
      return r;
    }
    // A fresh txn per try, so a late answer to an earlier try can't be taken for this one.
    do {
      p.txn = next_txn_++;
    } while (pending_.count({node, p.txn}) != 0);
    Values v;
    v["op"] = one(u(p.op));
    v["txn"] = one(u(p.txn));
    v["index"] = one(u(index));
    v["value"] = one(u(value));
    if (!send_msg(*m_service_, v, node, &r.error)) {
      r.status = Status::SendFailed;
      return r;
    }
    pending_[{node, p.txn}] = &p;
    cv_.wait_for(lk, std::chrono::milliseconds(cfg_.service_timeout_ms), [&] { return p.done || !running_; });
    pending_.erase({node, p.txn});
    if (p.done) {
      r.status = Status::Ok;
      const std::string st = enum_label("service_status", p.status);
      if (st != "ok") {
        static const std::map<std::string, Status> kMap = {
            {"bad_index", Status::BadIndex},     {"out_of_range", Status::OutOfRange},
            {"not_while_armed", Status::NotWhileArmed}, {"storage_error", Status::StorageError},
            {"read_only", Status::ReadOnly},     {"bad_op", Status::BadOp},
            {"busy", Status::Busy},              {"failed", Status::Failed}};
        auto it = kMap.find(st);
        r.status = it != kMap.end() ? it->second : Status::Failed;
        r.error = "node " + std::to_string(node) + " answered " + st;
      }
      r.value = p.value;  // raw; callers convert
      return r;
    }
  }
  r.status = Status::Timeout;
  r.error = "node " + std::to_string(node) + " didn't answer";
  return r;
}

Result Host::get(uint8_t node, const std::string& param) {
  Result r;
  const NodeType* t = nullptr;
  if ((r.status = check_client(node, &t, &r.error)) != Status::Ok) return r;
  const Param* p = find_param(node, param);
  if (p == nullptr) return {Status::UnknownName, 0, "no parameter " + param};
  r = request(node, "get", p->index, 0, cfg_.service_retries);
  if (r.ok()) r.value = value_from_wire(p->type, static_cast<uint32_t>(r.value));
  return r;
}

Result Host::set(uint8_t node, const std::string& param, double value) {
  Result r;
  const NodeType* t = nullptr;
  if ((r.status = check_client(node, &t, &r.error)) != Status::Ok) return r;
  const Param* p = find_param(node, param);
  if (p == nullptr) return {Status::UnknownName, 0, "no parameter " + param};
  if (p->access == "ro") return {Status::ReadOnly, 0, p->name + " is read-only"};
  if ((p->min && value < *p->min) || (p->max && value > *p->max)) {
    return {Status::OutOfRange, 0, p->name + " is outside its min/max"};
  }
  uint32_t wire = 0;
  if (!value_to_wire(p->type, value, wire, &r.error)) return {Status::BadValue, 0, p->name + ": " + r.error};
  r = request(node, "set", p->index, wire, cfg_.service_retries);
  // The node answers with the value it now holds (it may have rounded it).
  if (r.ok()) r.value = value_from_wire(p->type, static_cast<uint32_t>(r.value));
  return r;
}

Result Host::save(uint8_t node) {
  Result r;
  const NodeType* t = nullptr;
  if ((r.status = check_client(node, &t, &r.error)) != Status::Ok) return r;
  return request(node, "save", 0, 0, cfg_.service_retries);
}

Result Host::restore_defaults(uint8_t node) {
  Result r;
  const NodeType* t = nullptr;
  if ((r.status = check_client(node, &t, &r.error)) != Status::Ok) return r;
  return request(node, "restore", 0, 0, cfg_.service_retries);
}

Result Host::info(uint8_t node, const std::string& page) {
  Result r;
  const NodeType* t = nullptr;
  if ((r.status = check_client(node, &t, &r.error)) != Status::Ok) return r;
  const int64_t index = enum_value("info_page", page);
  if (index < 0) return {Status::UnknownName, 0, "no info page " + page};
  return request(node, "info", static_cast<uint16_t>(index), 0, cfg_.service_retries);
}

namespace {
const Message* find_stream(const NodeType& t, const std::string& name) {
  uint16_t port = 0;
  const Message* m = parse_index(name, port) ? t.message(port) : t.message(name);
  return m != nullptr && m->kind == "stream" ? m : nullptr;
}
}  // namespace

Result Host::get_rate(uint8_t node, const std::string& stream) {
  Result r;
  const NodeType* t = nullptr;
  if ((r.status = check_client(node, &t, &r.error)) != Status::Ok) return r;
  const Message* m = find_stream(*t, stream);
  if (m == nullptr) return {Status::UnknownName, 0, "no stream " + stream + " in " + t->name};
  r = request(node, "get_rate", m->port, 0, cfg_.service_retries);
  if (r.ok()) r.value /= 1000.0;  // mHz on the wire
  return r;
}

Result Host::set_rate(uint8_t node, const std::string& stream, double hz) {
  Result r;
  const NodeType* t = nullptr;
  if ((r.status = check_client(node, &t, &r.error)) != Status::Ok) return r;
  const Message* m = find_stream(*t, stream);
  if (m == nullptr) return {Status::UnknownName, 0, "no stream " + stream + " in " + t->name};
  if (!std::isfinite(hz) || hz < 0 || (m->rate && hz != 0 && (hz < m->rate->min || hz > m->rate->max)) ||
      (m->rate && hz == 0 && m->rate->min > 0)) {
    return {Status::OutOfRange, 0, m->name + ": rate outside its min/max"};
  }
  const double mhz = std::round(hz * 1000.0);
  if (mhz > std::numeric_limits<uint32_t>::max()) return {Status::OutOfRange, 0, "rate too high"};
  r = request(node, "set_rate", m->port, static_cast<uint32_t>(mhz), cfg_.service_retries);
  if (r.ok()) r.value /= 1000.0;
  return r;
}

Result Host::action(uint8_t node, const std::string& name, std::optional<double> arg) {
  Result r;
  const NodeType* t = nullptr;
  if ((r.status = check_client(node, &t, &r.error)) != Status::Ok) return r;
  const Action* a = t->action(name);
  if (a == nullptr) return {Status::UnknownName, 0, "no action " + name + " in " + t->name};
  uint32_t wire = 0;
  if (a->arg) {
    if (!arg) return {Status::BadValue, 0, name + " needs an argument"};
    if (!value_to_wire(*a->arg, *arg, wire, &r.error)) return {Status::BadValue, 0, name + ": " + r.error};
  } else if (arg) {
    return {Status::BadValue, 0, name + " takes no argument"};
  }
  // No retries: running an action twice isn't harmless the way reading a setting twice is.
  return request(node, "action", a->index, wire, 0);
}

// --- commands, NMT, E-stop ---------------------------------------------------------------

Result Host::send_command(uint8_t node, const std::string& message, const Values& values, bool raw) {
  return set_command(node, message, values, -1, raw);
}

Result Host::set_command(uint8_t node, const std::string& message, const Values& values, double hz, bool raw) {
  Result r;
  if (!running_) return {Status::NotRunning, 0, ""};
  std::lock_guard<std::mutex> lk(mu_);
  if (!election_.leading()) return {Status::NotLeader, 0, "only the leading host sends commands"};
  const NodeType* t = usable_type_locked(node);
  if (t == nullptr || !registry_.online(node)) return {Status::NoSuchNode, 0, "node " + std::to_string(node) + " is not online with a known type"};
  const Message* m = t->message(message);
  if (m == nullptr || m->kind != "command") return {Status::UnknownName, 0, "no command " + message + " in " + t->name};
  Frame f;
  EncodeOptions o;
  o.raw = raw;
  if (!encode_frame(*schema_, *m, values, cfg_.host_id, node, f, &r.error, o)) return {Status::BadValue, 0, r.error};
  if (!link_.send(f)) return {Status::SendFailed, 0, "link refused the frame"};
  if (hz >= 0) {
    const double rate = hz > 0 ? hz : cfg_.command_hz;
    Refresh& ref = commands_[{node, m->port}];
    ref.frame = f;
    ref.period_ns = period_ns(rate);
    ref.next_ns = now_ns() + ref.period_ns;
  }
  return r;
}

void Host::stop_command(uint8_t node, const std::string& message) {
  std::lock_guard<std::mutex> lk(mu_);
  const NodeInfo* n = registry_.find(node);
  const Message* m = n != nullptr && n->type != nullptr ? n->type->message(message) : nullptr;
  if (m != nullptr) commands_.erase({node, m->port});
}

void Host::stop_all_commands() {
  std::lock_guard<std::mutex> lk(mu_);
  commands_.clear();
}

Status Host::nmt(uint8_t node, const std::string& command) {
  if (!running_) return Status::NotRunning;
  const int64_t cmd = enum_value("nmt_command", command);
  if (cmd < 0) return Status::UnknownName;
  std::lock_guard<std::mutex> lk(mu_);
  if (!election_.leading()) return Status::NotLeader;
  Values v;
  v["command"] = one(u(static_cast<uint64_t>(cmd)));
  v["term"] = one(u(election_.term()));
  if (!send_msg(*m_nmt_, v, node)) return Status::SendFailed;
  if (command == "arm") armed_ = true;
  if (command == "disarm" && node == schema_->node_ids.broadcast) armed_ = false;
  return Status::Ok;
}

Status Host::arm(uint8_t node) { return nmt(node, "arm"); }
Status Host::disarm(uint8_t node) { return nmt(node, "disarm"); }

Status Host::estop(uint8_t reason) {
  std::lock_guard<std::mutex> lk(mu_);
  const Field* magic = m_estop_ != nullptr ? m_estop_->field("magic") : nullptr;
  if (magic == nullptr) return Status::SendFailed;
  // Latch locally first: whatever happens on the link, this host stops commanding.
  estop_latched_ = true;
  armed_ = false;
  commands_.clear();
  Values v;
  v["magic"] = one(u(magic->const_value.value_or(0)));
  v["reason"] = one(u(reason));
  return send_msg(*m_estop_, v, schema_->node_ids.broadcast) ? Status::Ok : Status::SendFailed;
}

Status Host::estop_release() {
  if (!running_) return Status::NotRunning;
  std::lock_guard<std::mutex> lk(mu_);
  if (!election_.leading()) return Status::NotLeader;
  Values v;
  v["term"] = one(u(election_.term()));
  if (!send_msg(*m_estop_release_, v, schema_->node_ids.broadcast)) return Status::SendFailed;
  estop_latched_ = false;
  return Status::Ok;
}

}  // namespace anv::host
