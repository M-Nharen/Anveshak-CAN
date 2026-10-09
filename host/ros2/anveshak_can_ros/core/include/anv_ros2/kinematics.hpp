// Skid-steer kinematics: a body twist (m/s, rad/s) -> per-side wheel speeds -> the six drive
// node targets. ROS-free so it can be unit-tested anywhere.
#pragma once

#include <array>
#include <string>
#include <vector>

#include "anv_gateway/gateway.hpp"

namespace anv::ros2 {

struct SkidSteerConfig {
  double wheel_radius = 0.15;  // m
  double track_width = 0.60;   // m, distance between the left and right wheel lines
  // Which drive nodes (1-6) sit on each side. Both motor channels of a node follow its side.
  std::vector<int> left_drives{1, 2, 3};
  std::vector<int> right_drives{4, 5, 6};
  bool invert_left = false;   // flip the sign if a side's motors are mounted mirrored
  bool invert_right = false;
  double accel_limit = 10.0;  // rad/s^2 sent with every command
};

// False (with *error set) for a non-positive radius or width, a drive outside 1-6, or a drive
// listed twice.
bool validate(const SkidSteerConfig& cfg, std::string* error);

struct SideSpeeds {
  double left = 0;   // wheel angular speed, rad/s
  double right = 0;
};

// v_left = v - w * track / 2, v_right = v + w * track / 2, then divide by the wheel radius.
SideSpeeds twist_to_side_speeds(double linear_mps, double angular_rps, const SkidSteerConfig& cfg);

// Drives not listed on either side get a zero target.
std::array<anv::gw::WheelTarget, anv::gw::kNumDrives> twist_to_targets(double linear_mps, double angular_rps,
                                                                       const SkidSteerConfig& cfg);

}  // namespace anv::ros2
