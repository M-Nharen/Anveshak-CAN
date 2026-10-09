# ROS-free part of the package: kinematics and the simulated rover, plus their tests.
# Builds standalone (cmake -S core) and as part of the colcon package.
cmake_minimum_required(VERSION 3.16)
if(NOT CMAKE_PROJECT_NAME)
  project(anv_ros2_core LANGUAGES C CXX)
endif()

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# The gateway library lives two packages over; it is not a colcon package, so build it in-tree.
if(NOT TARGET anv_gateway)
  set(ANV_GATEWAY_TESTS OFF CACHE BOOL "" FORCE)
  add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/../../../gateway ${CMAKE_CURRENT_BINARY_DIR}/gateway EXCLUDE_FROM_ALL)
endif()

add_library(anv_ros2_core STATIC src/kinematics.cpp src/sim_rover.cpp)
target_include_directories(anv_ros2_core PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_link_libraries(anv_ros2_core PUBLIC anv_gateway)
target_compile_options(anv_ros2_core PRIVATE -Wall -Wextra)
set_target_properties(anv_ros2_core PROPERTIES POSITION_INDEPENDENT_CODE ON)

if(BUILD_TESTING OR ANV_ROS2_CORE_TESTS)
  enable_testing()
  foreach(t test_kinematics test_sim_rover)
    add_executable(${t} tests/${t}.cpp)
    target_link_libraries(${t} PRIVATE anv_ros2_core)
    target_compile_options(${t} PRIVATE -Wall -Wextra)
    add_test(NAME ${t} COMMAND ${t})
    set_tests_properties(${t} PROPERTIES TIMEOUT 60)
  endforeach()
endif()
