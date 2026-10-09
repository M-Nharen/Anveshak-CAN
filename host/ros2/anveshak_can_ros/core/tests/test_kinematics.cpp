#include "anv_ros2/kinematics.hpp"
#include "test_support.hpp"

using namespace anv::ros2;

int main() {
  SkidSteerConfig cfg;
  cfg.wheel_radius = 0.2;
  cfg.track_width = 0.8;

  auto s = twist_to_side_speeds(1.0, 0.0, cfg);  // straight: both sides v / r
  CHECK_NEAR(s.left, 5.0, 1e-9);
  CHECK_NEAR(s.right, 5.0, 1e-9);

  s = twist_to_side_speeds(0.0, 1.0, cfg);  // spin left (counter-clockwise): right faster
  CHECK_NEAR(s.left, -0.4 / 0.2, 1e-9);
  CHECK_NEAR(s.right, 0.4 / 0.2, 1e-9);

  s = twist_to_side_speeds(1.0, 0.5, cfg);
  CHECK_NEAR(s.left, (1.0 - 0.2) / 0.2, 1e-9);
  CHECK_NEAR(s.right, (1.0 + 0.2) / 0.2, 1e-9);

  auto t = twist_to_targets(1.0, 0.0, cfg);
  for (int i = 0; i < 6; ++i) {
    CHECK_NEAR(t[i].a, 5.0, 1e-9);
    CHECK_NEAR(t[i].b, 5.0, 1e-9);  // both channels follow the side
  }

  cfg.invert_left = true;
  t = twist_to_targets(1.0, 0.0, cfg);
  CHECK_NEAR(t[0].a, -5.0, 1e-9);
  CHECK_NEAR(t[3].a, 5.0, 1e-9);

  cfg.left_drives = {1, 2};  // drive 3 unlisted -> zero
  t = twist_to_targets(1.0, 0.0, cfg);
  CHECK_NEAR(t[2].a, 0.0, 1e-12);
  CHECK_NEAR(t[2].b, 0.0, 1e-12);

  std::string err;
  SkidSteerConfig ok;
  CHECK(validate(ok, &err));
  SkidSteerConfig bad = ok;
  bad.wheel_radius = 0;
  CHECK(!validate(bad, &err));
  bad = ok;
  bad.track_width = -1;
  CHECK(!validate(bad, &err));
  bad = ok;
  bad.left_drives = {1, 7};
  CHECK(!validate(bad, &err));
  bad = ok;
  bad.right_drives = {3, 4};  // 3 is also on the left
  CHECK(!validate(bad, &err));

  if (failures()) return 1;
  std::puts("test_kinematics: ok");
  return 0;
}
