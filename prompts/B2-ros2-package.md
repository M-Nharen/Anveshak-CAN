# B2 — ROS 2 package (Humble and Jazzy)

**Who:** person B. **Needs:** B1 merged. **Folders:** `host/ros2/`, the ROS jobs in CI.

## Goal

A ROS 2 node that wraps the gateway library, so autonomy and teleop on the same host can drive
the rover and see its state. It must build and work on **both Humble and Jazzy** from the same
code.

## Read first

`docs/architecture.md` section "Host software": the ROS 2 interface table and "Humble and Jazzy,
on either host, at the same time".

## Build

1. `anveshak_can_msgs`: messages for drive feedback, node registry entry, bus status and events;
   services for arm, disarm, E-stop, request lead, param get/set, set debug (stub for now).
2. `anveshak_can_ros`: an `rclcpp` node that links the gateway library and:
   - subscribes `/cmd/drive` (six wheel targets) and also accepts `geometry_msgs/Twist` on
     `/cmd_vel`, converted with skid-steer kinematics (wheel radius and track width as ROS
     parameters)
   - publishes `/can/drive/feedback`, `/can/nodes`, `/can/bus`, and `/diagnostics`
     (`diagnostic_msgs`)
   - offers the services above, plus the parameter `debug_level` (stored only for now)
3. Handle Humble/Jazzy build differences in one place in CMake (checks on `ROS_DISTRO`). Use
   only stable `rclcpp` APIs.
4. A launch file and a config YAML per host (Orin, RPi).
5. CI: build and run tests in `ros:humble` and `ros:jazzy` containers (arm64 is optional if CI
   time is a problem; x86 is required).

## Done when

- `colcon build && colcon test` pass in both CI containers.
- A launch test (`launch_testing`) starts the node against the fake bus or the simulator,
  requests the lead, arms, publishes `/cmd_vel` and sees non-zero wheel feedback.

## Out of scope

Dashboard, gateway link, debug levels.
