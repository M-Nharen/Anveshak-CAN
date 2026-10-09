#include "anv_ros2/kinematics.hpp"

#include <set>

namespace anv::ros2 {

bool validate(const SkidSteerConfig& cfg, std::string* error) {
  auto fail = [&](const std::string& m) {
    if (error) *error = m;
    return false;
  };
  if (!(cfg.wheel_radius > 0)) return fail("wheel_radius must be > 0");
  if (!(cfg.track_width > 0)) return fail("track_width must be > 0");
  std::set<int> seen;
  for (const auto* side : {&cfg.left_drives, &cfg.right_drives}) {
    for (int d : *side) {
      if (d < 1 || d > static_cast<int>(anv::gw::kNumDrives)) return fail("drive " + std::to_string(d) + " is not 1-6");
      if (!seen.insert(d).second) return fail("drive " + std::to_string(d) + " is listed more than once");
    }
  }
  return true;
}

SideSpeeds twist_to_side_speeds(double linear_mps, double angular_rps, const SkidSteerConfig& cfg) {
  const double half = angular_rps * cfg.track_width / 2.0;
  SideSpeeds s;
  s.left = (linear_mps - half) / cfg.wheel_radius;
  s.right = (linear_mps + half) / cfg.wheel_radius;
  if (cfg.invert_left) s.left = -s.left;
  if (cfg.invert_right) s.right = -s.right;
  return s;
}

std::array<anv::gw::WheelTarget, anv::gw::kNumDrives> twist_to_targets(double linear_mps, double angular_rps,
                                                                       const SkidSteerConfig& cfg) {
  std::array<anv::gw::WheelTarget, anv::gw::kNumDrives> out;
  const SideSpeeds s = twist_to_side_speeds(linear_mps, angular_rps, cfg);
  auto fill = [&](const std::vector<int>& drives, double speed) {
    for (int d : drives) {
      if (d < 1 || d > static_cast<int>(anv::gw::kNumDrives)) continue;
      anv::gw::WheelTarget& t = out[static_cast<size_t>(d - 1)];
      t.mode = anv::gw::DriveMode::Velocity;
      t.a = speed;
      t.b = speed;
      t.accel = cfg.accel_limit;
    }
  };
  fill(cfg.left_drives, s.left);
  fill(cfg.right_drives, s.right);
  for (auto& t : out) t.accel = cfg.accel_limit;
  return out;
}

}  // namespace anv::ros2
