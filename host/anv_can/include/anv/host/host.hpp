// anv_host: a brain on the bus. Discovers nodes from their heartbeats, gives IDs to new
// boards, takes part in the leader election, and is the client for everything a host does to
// a node: settings, stream rates, commands, actions, arm/disarm, E-stop.
//
// Everything comes from the schema at run time. Nothing here knows how many nodes there are,
// what types they are, or which machine this host is: its ID, tie-break and allocator range
// come from HostConfig.
//
// Threading: start() runs one background thread that receives frames, keeps time and sends
// the periodic frames (host heartbeat, SYNC and refreshed commands while leading). Client
// calls (get/set/...) may come from any other thread; they block until the node answers.
// Callbacks run on the background thread with no lock held: keep them short, and don't make
// blocking client calls from them (those return WouldDeadlock). Set callbacks before start().
#pragma once

#include <atomic>
#include <condition_variable>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <vector>

#include "anv/codec.hpp"
#include "anv/host/allocator.hpp"
#include "anv/host/election.hpp"
#include "anv/host/registry.hpp"
#include "anv/link.hpp"
#include "anv/schema.hpp"

namespace anv::host {

enum class Status {
  Ok,
  // What the node answered (the protocol's service_status).
  BadIndex, OutOfRange, NotWhileArmed, StorageError, ReadOnly, BadOp, Busy, Failed,
  // Decided here, before or instead of asking the node.
  Timeout,        // the node didn't answer
  NoSuchNode,     // never seen, offline, or its type isn't usable (unknown / schema mismatch)
  UnknownName,    // no such parameter, stream, action or command in the node's type
  BadValue,       // doesn't fit the type, outside min/max, or wrong fields
  NotLeader,      // only the leading host may send this
  SendFailed,     // the link refused the frame
  NotRunning,     // start() not called, or stop()ped
  WouldDeadlock,  // a blocking call from a callback
};
const char* to_string(Status s);

struct Result {
  Status status = Status::Ok;
  double value = 0;   // the value read (get, get_rate in Hz) or the node's value after a set
  std::string error;  // for people
  bool ok() const { return status == Status::Ok; }
};

struct HostConfig {
  uint8_t host_id = 0;           // required: this host's node ID on the bus
  uint8_t tie_break = 0;         // lower wins a same-term clash; 0 = use host_id
  bool observer = false;         // never leads (tools that only watch)
  bool auto_takeover = false;    // claim the lead when the leader vanishes (off by default)

  // Off: never answer id_request. Auto: answer while leading, or while nobody leads and this
  // is the lowest-numbered host that could lead. Always: answer every request (one host only!).
  enum class Allocator { Off, Auto, Always } allocator = Allocator::Auto;
  uint8_t id_first = 0, id_last = 0;  // range to give out; 0 = the schema's node_ids
  std::string id_table_path;          // where MAC -> ID pairs are kept; empty = memory only

  double heartbeat_hz = 20;      // host_heartbeat rate
  double sync_hz = 50;           // SYNC rate while leading; 0 = don't send SYNC
  double command_hz = 50;        // default refresh rate for set_command()
  int leader_timeout_ms = 250;
  int node_timeout_ms = 500;
  int service_timeout_ms = 100;  // per try
  int service_retries = 2;       // extra tries for get/set/rate/save (not for actions)
  int tick_ms = 2;
};

// An event frame from a node (the protocol's `event` message), with its name looked up.
struct NodeEvent {
  uint8_t node = 0;
  uint16_t code = 0;
  std::string name;      // empty if neither the node's type nor the protocol knows the code
  std::string severity;  // label of the protocol's severity enum
  bool cleared = false;
  uint16_t detail = 0;
  uint16_t uptime_s = 0;
};

struct HostCallbacks {
  std::function<void(const NodeInfo&, bool returned)> node_appeared;
  std::function<void(const NodeInfo&)> node_lost;
  std::function<void(const NodeInfo&)> node_rebooted;
  std::function<void(const NodeInfo&, uint8_t old_state)> node_state;
  // A stream or event from a node's own type (port > protocol_last), decoded (physical values).
  std::function<void(uint8_t node, const Message&, const Values&, int64_t rx_ns)> telemetry;
  std::function<void(const NodeEvent&)> event;
  // A node confirmed a SET, from this host or any other.
  std::function<void(uint8_t node, const Param&, double value)> setting_changed;
  std::function<void(Role role, uint16_t term)> role_changed;
  std::function<void(uint8_t leader, uint16_t term)> leader_changed;  // 0 = no leader
  std::function<void(uint8_t leader)> leader_lost;
  std::function<void(uint8_t a, uint8_t b)> two_leaders;
  std::function<void(uint8_t node, uint64_t mac, uint16_t type_id)> id_assigned;
  std::function<void(uint8_t src, uint8_t reason)> estop;
};

// Parameter and action values ride in the service frame's 32-bit `value` as the C union
// anv_value_t: unsigned and bool as is, signed sign-extended, f32 as its IEEE bits.
bool value_to_wire(const ValueType& type, double value, uint32_t& wire, std::string* error);
double value_from_wire(const ValueType& type, uint32_t wire);

class Host {
 public:
  Host(Link& link, std::shared_ptr<const Schema> schema, HostConfig config);
  ~Host();
  Host(const Host&) = delete;
  Host& operator=(const Host&) = delete;

  bool set_callbacks(HostCallbacks callbacks);  // false once running
  bool start(std::string* error = nullptr);     // loads the ID table, starts the thread
  void stop();
  bool running() const { return running_; }

  // --- what's on the bus -------------------------------------------------------------
  std::vector<NodeInfo> nodes() const;
  std::optional<NodeInfo> node(uint8_t id) const;
  std::vector<PeerHost> hosts() const;  // other hosts heard
  std::vector<Assignment> id_table() const;
  // Block until node `id` is online (true) or the timeout passes.
  bool wait_for_node(uint8_t id, int timeout_ms);
  bool wait_for_lead(int timeout_ms);

  // --- leadership ----------------------------------------------------------------------
  Role role() const;
  uint16_t term() const;
  uint8_t leader() const;
  bool leading() const { return role() == Role::Leader; }
  // Operator choice: ask for `host_id` (default: this host) to lead. Goes on the bus so every
  // host hears it, and is applied here at once.
  Status request_lead(uint8_t host_id = 0);
  void resign();
  void set_operator_link(bool up);
  void set_autonomy_ready(bool ready);
  // Pin a board to an ID in this host's table (applies to its next id_request).
  bool pin_id(uint64_t mac, uint8_t node_id, uint16_t type_id, std::string* error);

  // --- settings (parameters), by name or index; the node's type or the protocol's -------
  Result get(uint8_t node, const std::string& param);
  Result set(uint8_t node, const std::string& param, double value);
  Result save(uint8_t node);
  Result restore_defaults(uint8_t node);
  Result info(uint8_t node, const std::string& page);  // a protocol info_page, raw u32 in value
  const Param* find_param(uint8_t node, const std::string& name_or_index) const;

  // --- streams -------------------------------------------------------------------------
  Result get_rate(uint8_t node, const std::string& stream);  // Hz
  Result set_rate(uint8_t node, const std::string& stream, double hz);  // 0 = off

  // --- actions -------------------------------------------------------------------------
  Result action(uint8_t node, const std::string& name, std::optional<double> arg = std::nullopt);

  // --- commands (leader only) ------------------------------------------------------------
  Result send_command(uint8_t node, const std::string& message, const Values& values, bool raw = false);
  // Keep sending it every 1/hz s while leading (0 = config.command_hz) until stop_command().
  // Dropped when this host stops leading or on E-stop: the next leader starts from nothing.
  Result set_command(uint8_t node, const std::string& message, const Values& values, double hz = 0,
                     bool raw = false);
  void stop_command(uint8_t node, const std::string& message);
  void stop_all_commands();

  // --- NMT, E-stop -------------------------------------------------------------------------
  Status arm(uint8_t node);     // 0 = every node; leader only
  Status disarm(uint8_t node);  // 0 = every node; leader only
  // Any nmt_command label (arm, disarm, reset, fault_reset, identify, forget_id).
  Status nmt(uint8_t node, const std::string& command);
  Status estop(uint8_t reason = 0);  // anyone, any time
  Status estop_release();            // leader only
  bool estop_latched() const;

  // --- diagnostics -----------------------------------------------------------------------
  // Turn debugging on node `node` (0 = every node) up or down; leader only, like nmt. level is
  // the protocol's 0-3; categories is a bitmask the node type defines meaning for (0: all);
  // timeout_s: 0 = until changed.
  Status set_debug_level(uint8_t node, uint8_t level, uint16_t categories = 0, uint16_t timeout_s = 0);

  const Schema& schema() const { return *schema_; }
  const HostConfig& config() const { return cfg_; }

 private:
  struct Pending {
    uint8_t node = 0, txn = 0;
    uint32_t op = 0;
    bool done = false;
    uint32_t status = 0, value = 0;
  };
  struct Refresh {
    Frame frame;
    int64_t period_ns = 0, next_ns = 0;
  };
  using Notify = std::vector<std::function<void()>>;

  void run();
  void handle(const Frame& f, int64_t now, Notify& out);
  void periodic(int64_t now, Notify& out);
  void on_election(const std::vector<ElectionEvent>& ev, int64_t now, Notify& out);
  void on_registry(const std::vector<RegistryEvent>& ev, Notify& out);
  void handle_id_request(uint8_t src, const Values& v, Notify& out);
  void handle_service(uint8_t src, uint8_t dst, const Values& v, Notify& out);
  bool should_allocate() const;
  bool send_msg(const Message& m, const Values& v, uint32_t dst, std::string* error = nullptr);
  void send_host_heartbeat(int64_t now);
  void drop_commands();

  Result request(uint8_t node, const char* op, uint16_t index, uint32_t value, int retries);
  Status check_client(uint8_t node, const NodeType** type, std::string* error) const;
  const NodeType* usable_type_locked(uint8_t node) const;
  Result service_result(const Pending& p, const ValueType* vt) const;

  int64_t enum_value(const char* enum_name, const std::string& label) const;
  std::string enum_label(const char* enum_name, int64_t value) const;

  Link& link_;
  std::shared_ptr<const Schema> schema_;
  HostConfig cfg_;
  HostCallbacks cb_;

  // Protocol messages, looked up by name once.
  const Message *m_estop_, *m_estop_release_, *m_sync_, *m_nmt_, *m_host_hb_, *m_lead_req_, *m_hb_,
      *m_id_req_, *m_id_assign_, *m_service_, *m_event_, *m_debug_level_;

  mutable std::mutex mu_;
  std::condition_variable cv_;
  Election election_;
  NodeRegistry registry_;
  IdAllocator allocator_;
  std::map<std::pair<uint8_t, uint8_t>, Pending*> pending_;
  std::map<std::pair<uint8_t, uint16_t>, Refresh> commands_;
  uint8_t next_txn_ = 0, lead_counter_ = 0, sync_counter_ = 0;
  int64_t start_ns_ = 0, next_hb_ns_ = 0, next_sync_ns_ = 0;
  bool estop_latched_ = false, armed_ = false, operator_link_ = false, autonomy_ready_ = false;

  std::atomic<bool> running_{false};
  std::thread thread_;
  std::thread::id thread_id_;
};

}  // namespace anv::host
