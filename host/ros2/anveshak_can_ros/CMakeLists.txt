cmake_minimum_required(VERSION 3.16)
project(anveshak_can_ros LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE RelWithDebInfo)
endif()

find_package(ament_cmake REQUIRED)
find_package(rclcpp REQUIRED)
find_package(rcl_interfaces REQUIRED)
find_package(geometry_msgs REQUIRED)
find_package(diagnostic_msgs REQUIRED)
find_package(builtin_interfaces REQUIRED)
find_package(anveshak_can_msgs REQUIRED)

include(cmake/anv_ros_compat.cmake)

# ROS-free part: kinematics, simulated rover, and the gateway library (built in-tree).
# Its tests are registered here too, so `colcon test` runs them.
add_subdirectory(core)

add_executable(can_gateway_node src/main.cpp src/gateway_node.cpp)
target_include_directories(can_gateway_node PRIVATE src)
# Plain signature: ament_target_dependencies uses it, and CMake forbids mixing the two forms.
target_link_libraries(can_gateway_node anv_ros2_core)
target_compile_options(can_gateway_node PRIVATE -Wall -Wextra)
anv_link_ros(can_gateway_node)

install(TARGETS can_gateway_node DESTINATION lib/${PROJECT_NAME})
install(DIRECTORY launch config DESTINATION share/${PROJECT_NAME})

if(BUILD_TESTING)
  find_package(launch_testing_ament_cmake REQUIRED)
  add_launch_test(test/test_drive.launch.py TIMEOUT 90)
endif()

ament_package()
