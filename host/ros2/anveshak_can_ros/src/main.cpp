#include <memory>

#include "gateway_node.hpp"
#include "rclcpp/rclcpp.hpp"

int main(int argc, char** argv) {
  rclcpp::init(argc, argv);
  auto node = std::make_shared<anv::ros2::GatewayNode>();
  if (!node->ok()) {
    rclcpp::shutdown();
    return 1;
  }
  // Callbacks (services, subscriptions, timers) can overlap; the node protects its own state.
  rclcpp::executors::MultiThreadedExecutor exec;
  exec.add_node(node);
  exec.spin();
  node->shutdown();
  rclcpp::shutdown();
  return 0;
}
