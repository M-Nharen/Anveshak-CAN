// ROS 2 wrapper around the anv_gateway library. Thin on purpose: conversion and plumbing only;
// the CAN logic is in the gateway, the maths in anv_ros2/kinematics.hpp.
#pragma once

#include <array>
#include <chrono>
#include <deque>
#include <memory>
#include <mutex>
#include <string>

#include "anv_gateway/gateway.hpp"
#include "anv_gateway/gateway_link.hpp"
#include "anv_ros2/kinematics.hpp"
#include "anv_ros2/sim_rover.hpp"
#include "anveshak_can_msgs/msg/bus_status.hpp"
#include "anveshak_can_msgs/msg/can_error_event.hpp"
#include "anveshak_can_msgs/msg/can_event.hpp"
#include "anveshak_can_msgs/msg/drive_feedback.hpp"
#include "anveshak_can_msgs/msg/drive_targets.hpp"
#include "anveshak_can_msgs/msg/node_registry.hpp"
#include "anveshak_can_msgs/srv/arm.hpp"
#include "anveshak_can_msgs/srv/disarm.hpp"
#include "anveshak_can_msgs/srv/estop.hpp"
#include "anveshak_can_msgs/srv/param_get.hpp"
#include "anveshak_can_msgs/srv/param_set.hpp"
#include "anveshak_can_msgs/srv/request_lead.hpp"
#include "anveshak_can_msgs/srv/set_debug.hpp"
#include "diagnostic_msgs/msg/diagnostic_array.hpp"
#include "geometry_msgs/msg/twist.hpp"
#include "rcl_interfaces/msg/set_parameters_result.hpp"
#include "rclcpp/rclcpp.hpp"

namespace anv::ros2 {

class GatewayNode : public rclcpp::Node {
 public:
  GatewayNode();
  ~GatewayNode() override;

  bool ok() const { return ok_; }
  void shutdown();  // stops the gateway (and the simulated rover, if any)

 private:
  using Clock = std::chrono::steady_clock;
  template <typename S>
  using Req = std::shared_ptr<typename S::Request>;
  template <typename S>
  using Res = std::shared_ptr<typename S::Response>;

  bool setup_gateway();
  void declare_parameters();

  void on_cmd_drive(const anveshak_can_msgs::msg::DriveTargets::ConstSharedPtr msg);
  void on_cmd_vel(const geometry_msgs::msg::Twist::ConstSharedPtr msg);
  void store_targets(const std::array<anv::gw::WheelTarget, anv::gw::kNumDrives>& t);
  void on_drive_timer();
  void on_status_timer();
  void on_event(const anv::gw::GatewayEvent& e);
  rcl_interfaces::msg::SetParametersResult on_set_parameters(const std::vector<rclcpp::Parameter>& params);

  static void answer(const anv::gw::CmdResult r, const std::string& what, bool* success, std::string* message);

  bool ok_ = false;
  std::string bus_kind_;
  double cmd_timeout_s_ = 0.5;
  SkidSteerConfig kin_;

  std::unique_ptr<SimulatedRover> sim_;
  std::unique_ptr<anv::gw::Gateway> gateway_;
  std::unique_ptr<anv::gw::GatewayLink> link_;
  int event_sub_ = 0;

  std::mutex mu_;  // guards targets_, events_, error_events_
  std::array<anv::gw::WheelTarget, anv::gw::kNumDrives> targets_{};
  Clock::time_point targets_time_{};
  bool have_targets_ = false;
  bool zero_sent_ = true;
  std::deque<anveshak_can_msgs::msg::CanEvent> events_;
  std::deque<anveshak_can_msgs::msg::CanErrorEvent> error_events_;

  rclcpp::Subscription<anveshak_can_msgs::msg::DriveTargets>::SharedPtr sub_drive_;
  rclcpp::Subscription<geometry_msgs::msg::Twist>::SharedPtr sub_vel_;
  rclcpp::Publisher<anveshak_can_msgs::msg::DriveFeedback>::SharedPtr pub_feedback_;
  rclcpp::Publisher<anveshak_can_msgs::msg::NodeRegistry>::SharedPtr pub_nodes_;
  rclcpp::Publisher<anveshak_can_msgs::msg::BusStatus>::SharedPtr pub_bus_;
  rclcpp::Publisher<anveshak_can_msgs::msg::CanEvent>::SharedPtr pub_events_;
  rclcpp::Publisher<anveshak_can_msgs::msg::CanErrorEvent>::SharedPtr pub_errors_;
  rclcpp::Publisher<diagnostic_msgs::msg::DiagnosticArray>::SharedPtr pub_diag_;
  rclcpp::TimerBase::SharedPtr drive_timer_;
  rclcpp::TimerBase::SharedPtr status_timer_;

  rclcpp::Service<anveshak_can_msgs::srv::Arm>::SharedPtr srv_arm_;
  rclcpp::Service<anveshak_can_msgs::srv::Disarm>::SharedPtr srv_disarm_;
  rclcpp::Service<anveshak_can_msgs::srv::Estop>::SharedPtr srv_estop_;
  rclcpp::Service<anveshak_can_msgs::srv::RequestLead>::SharedPtr srv_lead_;
  rclcpp::Service<anveshak_can_msgs::srv::ParamGet>::SharedPtr srv_param_get_;
  rclcpp::Service<anveshak_can_msgs::srv::ParamSet>::SharedPtr srv_param_set_;
  rclcpp::Service<anveshak_can_msgs::srv::SetDebug>::SharedPtr srv_debug_;
  OnSetParametersCallbackHandle::SharedPtr param_cb_;
};

}  // namespace anv::ros2
