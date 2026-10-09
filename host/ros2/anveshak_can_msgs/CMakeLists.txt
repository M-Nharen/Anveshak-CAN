cmake_minimum_required(VERSION 3.16)
project(anveshak_can_msgs)

find_package(ament_cmake REQUIRED)
find_package(rosidl_default_generators REQUIRED)
find_package(builtin_interfaces REQUIRED)
find_package(std_msgs REQUIRED)

rosidl_generate_interfaces(${PROJECT_NAME}
  "msg/WheelTarget.msg"
  "msg/DriveTargets.msg"
  "msg/WheelFeedback.msg"
  "msg/DriveFeedback.msg"
  "msg/NodeStatus.msg"
  "msg/MessageRate.msg"
  "msg/NodeRegistry.msg"
  "msg/BusStatus.msg"
  "msg/CanEvent.msg"
  "srv/Arm.srv"
  "srv/Disarm.srv"
  "srv/Estop.srv"
  "srv/RequestLead.srv"
  "srv/ParamGet.srv"
  "srv/ParamSet.srv"
  "srv/SetDebug.srv"
  DEPENDENCIES std_msgs builtin_interfaces
)

ament_export_dependencies(rosidl_default_runtime)
ament_package()
