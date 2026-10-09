# Everything that differs between ROS 2 distributions lives here, so the C++ sources stay the
# same on Humble and Jazzy. Only stable rclcpp APIs are used; the defines below exist for the day
# a real difference shows up.
if(NOT DEFINED ENV{ROS_DISTRO})
  message(FATAL_ERROR "ROS_DISTRO is not set: source your ROS 2 installation first")
endif()

set(ANV_ROS_DISTRO "$ENV{ROS_DISTRO}")
if(ANV_ROS_DISTRO STREQUAL "humble")
  set(ANV_ROS_COMPILE_DEFINITIONS ANV_ROS_HUMBLE=1)
elseif(ANV_ROS_DISTRO STREQUAL "jazzy")
  set(ANV_ROS_COMPILE_DEFINITIONS ANV_ROS_JAZZY=1)
else()
  message(WARNING "Untested ROS distro '${ANV_ROS_DISTRO}'; building as if Jazzy")
  set(ANV_ROS_COMPILE_DEFINITIONS ANV_ROS_JAZZY=1)
endif()
message(STATUS "anveshak_can_ros: building for ROS 2 ${ANV_ROS_DISTRO}")

# Link a target against the ROS libraries the node needs.
macro(anv_link_ros target)
  ament_target_dependencies(${target} rclcpp rcl_interfaces geometry_msgs diagnostic_msgs builtin_interfaces anveshak_can_msgs)
  target_compile_definitions(${target} PRIVATE ${ANV_ROS_COMPILE_DEFINITIONS})
endmacro()
