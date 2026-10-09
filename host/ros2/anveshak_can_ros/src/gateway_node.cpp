#include "gateway_node.hpp"

#include <algorithm>
#include <functional>
#include <utility>
#include <vector>

#include "anv_gateway/messages.hpp"
#include "anv_gateway/socketcan_bus.hpp"
#include "anv_gateway/status.hpp"
#include "diagnostic_msgs/msg/diagnostic_status.hpp"
#include "diagnostic_msgs/msg/key_value.hpp"

namespace anv::ros2 {

namespace msgs = anveshak_can_msgs;
using anv::gw::CmdResult;
using namespace std::chrono_literals;

namespace {

builtin_interfaces::msg::Time to_time(int64_t ns) {
  builtin_interfaces::msg::Time t;
  t.sec = static_cast<int32_t>(ns / 1000000000LL);
  t.nanosec = static_cast<uint32_t>(ns % 1000000000LL);
  return t;
}

msgs::msg::NodeStatus to_msg(const anv::gw::NodeInfo& n) {
  msgs::msg::NodeStatus m;
  m.node_id = n.node_id;
  m.name = n.name;
  m.seen = n.seen;
  m.alive = n.alive;
  m.state = n.state;
  m.node_type = n.node_type;
  m.protocol_version = n.protocol_version;
  m.can_error_state = n.can_error_state;
  m.bootloader_running = n.bootloader_running;
  m.unsaved_param_changes = n.unsaved_param_changes;
  m.debug_level = n.debug_level;
  m.fw_build_id = n.fw_build_id;
  m.uptime_s = n.uptime_s;
  m.reboot_count = n.reboot_count;
  m.tec = n.tec;
  m.rec = n.rec;
  return m;
}

}  // namespace

GatewayNode::GatewayNode() : rclcpp::Node("can_gateway") {
  declare_parameters();
  if (!setup_gateway()) return;

  // Topics. Commands use the default reliable QoS with a small queue: a lost command is replaced
  // by the next one anyway, and the node re-sends the latest target at 50 Hz.
  sub_drive_ = create_subscription<msgs::msg::DriveTargets>(
      "/cmd/drive", rclcpp::QoS(10), std::bind(&GatewayNode::on_cmd_drive, this, std::placeholders::_1));
  sub_vel_ = create_subscription<geometry_msgs::msg::Twist>(
      "/cmd_vel", rclcpp::QoS(10), std::bind(&GatewayNode::on_cmd_vel, this, std::placeholders::_1));
  pub_feedback_ = create_publisher<msgs::msg::DriveFeedback>("/can/drive/feedback", rclcpp::QoS(10));
  pub_nodes_ = create_publisher<msgs::msg::NodeRegistry>("/can/nodes", rclcpp::QoS(10));
  pub_bus_ = create_publisher<msgs::msg::BusStatus>("/can/bus", rclcpp::QoS(10));
  pub_events_ = create_publisher<msgs::msg::CanEvent>("/can/events", rclcpp::QoS(50));
  pub_diag_ = create_publisher<diagnostic_msgs::msg::DiagnosticArray>("/diagnostics", rclcpp::QoS(10));

  // Services are named can/... (so /can/arm with no namespace).
  srv_arm_ = create_service<msgs::srv::Arm>(
      "can/arm", [this](Req<msgs::srv::Arm>, Res<msgs::srv::Arm> res) {
        answer(gateway_->arm(), "arm", &res->success, &res->message);
      });
  srv_disarm_ = create_service<msgs::srv::Disarm>(
      "can/disarm", [this](Req<msgs::srv::Disarm>, Res<msgs::srv::Disarm> res) {
        answer(gateway_->disarm(), "disarm", &res->success, &res->message);
      });
  srv_estop_ = create_service<msgs::srv::Estop>(
      "can/estop", [this](Req<msgs::srv::Estop> req, Res<msgs::srv::Estop> res) {
        if (req->release)
          answer(gateway_->release_estop(), "estop release", &res->success, &res->message);
        else
          answer(gateway_->estop(), "estop", &res->success, &res->message);
      });
  srv_lead_ = create_service<msgs::srv::RequestLead>(
      "can/request_lead", [this](Req<msgs::srv::RequestLead> req, Res<msgs::srv::RequestLead> res) {
        answer(gateway_->request_lead(req->host_id), "request lead", &res->success, &res->message);
      });
  // Stubs: the parameter service and the debug levels are separate pieces of work.
  srv_param_get_ = create_service<msgs::srv::ParamGet>(
      "can/param_get", [](Req<msgs::srv::ParamGet>, Res<msgs::srv::ParamGet> res) {
        res->success = false;
        res->message = "not implemented yet";
        res->value_raw = 0;
      });
  srv_param_set_ = create_service<msgs::srv::ParamSet>(
      "can/param_set", [](Req<msgs::srv::ParamSet>, Res<msgs::srv::ParamSet> res) {
        res->success = false;
        res->message = "not implemented yet";
      });
  srv_debug_ = create_service<msgs::srv::SetDebug>(
      "can/set_debug", [](Req<msgs::srv::SetDebug>, Res<msgs::srv::SetDebug> res) {
        res->success = false;
        res->message = "not implemented yet";
      });

  // 50 Hz refresh: the gateway holds a target for 100 ms, so the node re-sends the latest one
  // until it is older than cmd_timeout_s.
  drive_timer_ = create_wall_timer(20ms, std::bind(&GatewayNode::on_drive_timer, this));
  const double rate = std::max(1.0, get_parameter("status_rate_hz").as_double());
  status_timer_ = create_wall_timer(std::chrono::duration_cast<std::chrono::nanoseconds>(
                                        std::chrono::duration<double>(1.0 / rate)),
                                    std::bind(&GatewayNode::on_status_timer, this));
  ok_ = true;
}

GatewayNode::~GatewayNode() { shutdown(); }

void GatewayNode::shutdown() {
  if (gateway_) {
    if (event_sub_) gateway_->unsubscribe(event_sub_);
    event_sub_ = 0;
    gateway_->stop();
  }
  if (sim_) sim_->stop();
}

void GatewayNode::declare_parameters() {
  declare_parameter<std::string>("bus", "socketcan");  // "socketcan" or "fake" (in-process rover)
  declare_parameter<std::string>("interface_name", "can0");
  declare_parameter<int>("host_id", 1);
  declare_parameter<int>("tie_break", 0);
  declare_parameter<bool>("auto_takeover", false);
  declare_parameter<int>("rt_priority", 50);
  declare_parameter<int>("cpu_affinity", -1);

  declare_parameter<double>("wheel_radius", 0.15);
  declare_parameter<double>("track_width", 0.60);
  declare_parameter<std::vector<int64_t>>("left_drives", std::vector<int64_t>{1, 2, 3});
  declare_parameter<std::vector<int64_t>>("right_drives", std::vector<int64_t>{4, 5, 6});
  declare_parameter<bool>("invert_left", false);
  declare_parameter<bool>("invert_right", false);
  declare_parameter<double>("accel_limit", 10.0);
  declare_parameter<double>("cmd_timeout_s", 0.5);
  declare_parameter<double>("status_rate_hz", 10.0);

  // Stored only: the debug-level mechanism is not built yet.
  declare_parameter<int>("debug_level", 0);
}

bool GatewayNode::setup_gateway() {
  bus_kind_ = get_parameter("bus").as_string();
  cmd_timeout_s_ = get_parameter("cmd_timeout_s").as_double();

  kin_.wheel_radius = get_parameter("wheel_radius").as_double();
  kin_.track_width = get_parameter("track_width").as_double();
  kin_.invert_left = get_parameter("invert_left").as_bool();
  kin_.invert_right = get_parameter("invert_right").as_bool();
  kin_.accel_limit = get_parameter("accel_limit").as_double();
  kin_.left_drives.clear();
  kin_.right_drives.clear();
  // Keep the Parameter objects alive: as_integer_array() returns a reference into them.
  const rclcpp::Parameter left = get_parameter("left_drives");
  const rclcpp::Parameter right = get_parameter("right_drives");
  for (auto d : left.as_integer_array()) kin_.left_drives.push_back(static_cast<int>(d));
  for (auto d : right.as_integer_array()) kin_.right_drives.push_back(static_cast<int>(d));
  std::string err;
  if (!validate(kin_, &err)) {
    RCLCPP_FATAL(get_logger(), "bad kinematics parameters: %s", err.c_str());
    return false;
  }
  if (cmd_timeout_s_ <= 0) {
    RCLCPP_FATAL(get_logger(), "cmd_timeout_s must be positive");
    return false;
  }

  anv::gw::GatewayConfig cfg;
  cfg.interface_name = get_parameter("interface_name").as_string();
  cfg.host_id = static_cast<uint8_t>(get_parameter("host_id").as_int());
  cfg.tie_break = static_cast<uint8_t>(get_parameter("tie_break").as_int());
  cfg.auto_takeover = get_parameter("auto_takeover").as_bool();
  cfg.rt_priority = static_cast<int>(get_parameter("rt_priority").as_int());
  cfg.cpu_affinity = static_cast<int>(get_parameter("cpu_affinity").as_int());

  std::unique_ptr<anv::gw::ICanBus> bus;
  if (bus_kind_ == "fake") {
    sim_ = std::make_unique<SimulatedRover>();
    bus = sim_->connect();
    cfg.rt_priority = 0;  // a test/dev setup: no need for real-time scheduling
  } else if (bus_kind_ == "socketcan") {
    bus = anv::gw::SocketCanBus::open(cfg.interface_name, &err);
    if (!bus) {
      RCLCPP_FATAL(get_logger(), "cannot open %s: %s", cfg.interface_name.c_str(), err.c_str());
      return false;
    }
  } else {
    RCLCPP_FATAL(get_logger(), "parameter 'bus' must be 'socketcan' or 'fake', got '%s'", bus_kind_.c_str());
    return false;
  }

  gateway_ = std::make_unique<anv::gw::Gateway>(cfg, std::move(bus));
  gateway_->set_logger([this](const std::string& m) { RCLCPP_INFO(get_logger(), "%s", m.c_str()); });
  event_sub_ = gateway_->subscribe([this](const anv::gw::GatewayEvent& e) { on_event(e); });
  if (sim_) sim_->start();
  if (!gateway_->start()) {
    RCLCPP_FATAL(get_logger(), "gateway failed to start");
    return false;
  }
  RCLCPP_INFO(get_logger(), "gateway started: host %d on %s", cfg.host_id,
              bus_kind_ == "fake" ? "simulated bus" : cfg.interface_name.c_str());
  return true;
}

void GatewayNode::store_targets(const std::array<anv::gw::WheelTarget, anv::gw::kNumDrives>& t) {
  std::lock_guard<std::mutex> lock(mu_);
  targets_ = t;
  targets_time_ = Clock::now();
  have_targets_ = true;
  zero_sent_ = false;
}

void GatewayNode::on_cmd_vel(const geometry_msgs::msg::Twist::ConstSharedPtr msg) {
  store_targets(twist_to_targets(msg->linear.x, msg->angular.z, kin_));
}

void GatewayNode::on_cmd_drive(const msgs::msg::DriveTargets::ConstSharedPtr msg) {
  std::array<anv::gw::WheelTarget, anv::gw::kNumDrives> t{};
  for (size_t i = 0; i < t.size(); ++i) {
    const auto& w = msg->wheels[i];
    t[i].mode = static_cast<anv::gw::DriveMode>(std::min<uint8_t>(w.mode, 3));
    t[i].a = w.channel_a;
    t[i].b = w.channel_b;
    t[i].accel = w.accel_limit;
  }
  store_targets(t);
}

void GatewayNode::on_drive_timer() {
  std::array<anv::gw::WheelTarget, anv::gw::kNumDrives> t;
  {
    std::lock_guard<std::mutex> lock(mu_);
    if (!have_targets_) return;
    const bool fresh = std::chrono::duration<double>(Clock::now() - targets_time_).count() < cmd_timeout_s_;
    if (!fresh) {
      if (zero_sent_) return;  // already told the gateway to stop
      for (auto& w : targets_) {
        w.mode = anv::gw::DriveMode::Velocity;
        w.a = w.b = 0;
      }
      zero_sent_ = true;
    }
    t = targets_;
  }
  gateway_->set_targets(t);
}

void GatewayNode::on_event(const anv::gw::GatewayEvent& e) {
  msgs::msg::CanEvent m;
  m.stamp = to_time(e.time_ns);
  m.type = static_cast<uint8_t>(e.type);
  m.name = anv::gw::event_type_name(e.type);
  m.node_id = e.node_id;
  m.term = e.term;
  m.role = e.role;
  std::lock_guard<std::mutex> lock(mu_);
  events_.push_back(m);
  if (events_.size() > 200) events_.pop_front();
}

void GatewayNode::on_status_timer() {
  const anv::gw::GatewayState s = gateway_->state();
  const auto stamp = now();

  msgs::msg::DriveFeedback fb;
  fb.header.stamp = stamp;
  fb.header.frame_id = "base_link";
  fb.host_id = s.host_id;
  fb.role = static_cast<uint8_t>(s.role);
  fb.term = s.term;

  msgs::msg::NodeRegistry reg;
  reg.header.stamp = stamp;
  reg.host_id = s.host_id;
  reg.role = static_cast<uint8_t>(s.role);
  reg.term = s.term;

  for (const auto& n : s.nodes) {
    reg.nodes.push_back(to_msg(n));
    if (n.node_id < 4 || n.node_id > 9) continue;  // drive nodes 1-6 = node IDs 4-9
    const size_t i = n.node_id - 4;
    auto& w = fb.wheels[i];
    w.drive = static_cast<uint8_t>(i + 1);
    w.node_id = n.node_id;
    w.alive = n.alive;
    w.state = n.state;
    w.has_feedback = n.has_fb_a;
    if (n.has_fb_a) {
      w.state = n.fb_a.state;
      w.fault_flags = n.fb_a.fault_flags;
      w.speed_a = anv::gw::speed_rad_s(n.fb_a.speed_a);
      w.speed_b = anv::gw::speed_rad_s(n.fb_a.speed_b);
      w.current_a = anv::gw::current_a(n.fb_a.current_a);
      w.current_b = anv::gw::current_a(n.fb_a.current_b);
    }
    w.has_encoders = n.has_fb_b;
    if (n.has_fb_b) {
      w.encoder_a = n.fb_b.encoder_a;
      w.encoder_b = n.fb_b.encoder_b;
    }
    w.has_telemetry = n.has_telem0;
    if (n.has_telem0) {
      w.supply_voltage_v = n.telem0.supply_voltage_mv / 1000.0;
      w.temp_driver_a = n.telem0.temp_driver_a;
      w.temp_driver_b = n.telem0.temp_driver_b;
      w.temp_mcu = n.telem0.temp_mcu;
    }
    if (n.has_telem1) {
      w.duty_a = anv::gw::duty_percent(n.telem1.duty_a);
      w.duty_b = anv::gw::duty_percent(n.telem1.duty_b);
    }
  }
  for (size_t i = 0; i < fb.wheels.size(); ++i) {  // drives never heard from keep their number
    if (fb.wheels[i].drive == 0) {
      fb.wheels[i].drive = static_cast<uint8_t>(i + 1);
      fb.wheels[i].node_id = static_cast<uint8_t>(4 + i);
    }
  }
  for (const auto& r : s.rates) {
    msgs::msg::MessageRate m;
    m.can_id = r.id;
    m.node_id = r.node_id;
    m.measured_hz = static_cast<float>(r.measured_hz);
    m.expected_hz = static_cast<float>(r.expected_hz);
    m.low = r.low;
    reg.rates.push_back(m);
  }

  msgs::msg::BusStatus bus;
  bus.header.stamp = stamp;
  bus.host_id = s.host_id;
  bus.role = static_cast<uint8_t>(s.role);
  bus.term = s.term;
  bus.believed_master = s.believed_master;
  bus.leader_lost = s.leader_lost;
  bus.armed = s.armed;
  bus.estop_latched = s.estop_latched;
  bus.bus_off = s.bus.bus_off;
  bus.rx_frames = s.bus.rx_frames;
  bus.tx_frames = s.bus.tx_frames;
  bus.tx_failures = s.bus.tx_failures;
  bus.error_frames = s.bus.error_frames;
  bus.undecoded_frames = s.bus.undecoded_frames;
  bus.rx_fps = static_cast<float>(s.bus.rx_fps);
  bus.cycles_sent = s.cycles_sent;
  bus.late_cycles = s.late_cycles;
  bus.worst_lateness_us = static_cast<uint32_t>(std::max<int64_t>(0, s.worst_lateness_ns / 1000));

  // Diagnostics: one entry for the gateway, one per node that has been seen.
  using DS = diagnostic_msgs::msg::DiagnosticStatus;
  auto kv = [](const std::string& k, const std::string& v) {
    diagnostic_msgs::msg::KeyValue x;
    x.key = k;
    x.value = v;
    return x;
  };
  diagnostic_msgs::msg::DiagnosticArray diag;
  diag.header.stamp = stamp;
  {
    DS g;
    g.name = "can_gateway";
    g.hardware_id = "host" + std::to_string(s.host_id);
    g.level = (s.bus.bus_off || s.estop_latched) ? DS::ERROR : (s.leader_lost || s.late_cycles > 0 ? DS::WARN : DS::OK);
    g.message = std::string("role ") + anv::gw::role_name(s.role);
    g.values.push_back(kv("term", std::to_string(s.term)));
    g.values.push_back(kv("armed", s.armed ? "true" : "false"));
    g.values.push_back(kv("estop_latched", s.estop_latched ? "true" : "false"));
    g.values.push_back(kv("late_cycles", std::to_string(s.late_cycles)));
    diag.status.push_back(g);
  }
  for (const auto& n : s.nodes) {
    if (!n.seen) continue;
    DS d;
    d.name = "can_gateway/" + n.name;
    d.hardware_id = "node" + std::to_string(n.node_id);
    if (!n.alive)
      d.level = DS::ERROR;
    else if (n.state == 3 || n.state == 4)
      d.level = DS::ERROR;
    else
      d.level = DS::OK;
    d.message = n.alive ? anv::gw::node_state_name(n.state) : "missing";
    d.values.push_back(kv("uptime_s", std::to_string(n.uptime_s)));
    d.values.push_back(kv("reboots", std::to_string(n.reboot_count)));
    d.values.push_back(kv("fw_build_id", std::to_string(n.fw_build_id)));
    diag.status.push_back(d);
  }

  pub_feedback_->publish(fb);
  pub_nodes_->publish(reg);
  pub_bus_->publish(bus);
  pub_diag_->publish(diag);

  std::deque<msgs::msg::CanEvent> events;
  {
    std::lock_guard<std::mutex> lock(mu_);
    events.swap(events_);
  }
  for (const auto& e : events) pub_events_->publish(e);
}

void GatewayNode::answer(const CmdResult r, const std::string& what, bool* success, std::string* message) {
  *success = (r == CmdResult::Ok);
  *message = *success ? what + ": ok" : what + ": " + anv::gw::cmd_result_name(r);
}

}  // namespace anv::ros2
