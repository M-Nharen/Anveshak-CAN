# ROS 2 packages

| Package | What |
|---|---|
| `anveshak_can_msgs` | Messages and services (decoded drive feedback, node registry, bus status, events, commands). |
| `anveshak_can_ros` | `can_gateway_node`: wraps the `host/gateway` library. Humble and Jazzy build from the same code. |

## Build and test

```
source /opt/ros/<humble|jazzy>/setup.bash
colcon build --base-paths host/ros2
colcon test  --base-paths host/ros2 && colcon test-result --verbose
```

`colcon test` runs the ROS-free unit tests (kinematics, simulated rover) and a `launch_testing`
test that starts the node on the in-process simulated rover (`bus: fake`), requests the lead, arms,
publishes `/cmd_vel` and checks non-zero wheel feedback.

The ROS-free part also builds without ROS:
`cmake -S host/ros2/anveshak_can_ros/core -B build -DANV_ROS2_CORE_TESTS=ON && cmake --build build && ctest --test-dir build`.

## Run

```
ros2 launch anveshak_can_ros can_gateway.launch.py host:=orin          # or host:=rpi
ros2 launch anveshak_can_ros can_gateway.launch.py host:=orin bus:=fake  # no hardware
ros2 service call /can/request_lead anveshak_can_msgs/srv/RequestLead "{host_id: 1}"
ros2 service call /can/arm anveshak_can_msgs/srv/Arm
ros2 topic pub -r 10 /cmd_vel geometry_msgs/msg/Twist "{linear: {x: 0.3}}"
```

## Interfaces

- Subscribes: `/cmd/drive` (`DriveTargets`, six wheel targets), `/cmd_vel` (`Twist`, skid-steer with
  `wheel_radius`, `track_width`, `left_drives`, `right_drives`). The newest command from either wins; the node
  re-sends it at 50 Hz and stops the wheels after `cmd_timeout_s` without a new one.
- Publishes: `/can/drive/feedback`, `/can/nodes`, `/can/bus`, `/can/events`, `/diagnostics`.
- Services: `/can/arm`, `/can/disarm`, `/can/estop` (`release: true` sends ESTOP_RELEASE),
  `/can/request_lead`. Stubs that answer `success: false`: `/can/param_get`, `/can/param_set`, `/can/set_debug`.
- Parameters: see `config/orin.yaml` and `config/rpi.yaml`. `debug_level` is stored only.

Distro differences live in `anveshak_can_ros/cmake/anv_ros_compat.cmake` (a check on `ROS_DISTRO`).
