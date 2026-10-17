cmake_minimum_required(VERSION 3.16)
project(anv_can VERSION 0.1.0 LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_C_STANDARD 11)
if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
  set(CMAKE_BUILD_TYPE RelWithDebInfo)
endif()

option(ANV_WERROR "Treat warnings as errors" OFF)
option(ANV_CAN_TESTS "Build the C++ tests" ON)
option(ANV_CAN_PYTHON "Build the Python module (needs pybind11)" OFF)

set(ANV_REPO_ROOT ${CMAKE_CURRENT_SOURCE_DIR}/../..)
set(ANV_SCHEMA_JSON ${ANV_REPO_ROOT}/schema/generated/schema.json)
set(ANV_SCHEMA_C ${ANV_REPO_ROOT}/schema/generated/c)

find_package(Threads REQUIRED)

set(ANV_WARNINGS -Wall -Wextra -Wshadow -Wconversion -Wno-sign-conversion)

# anv_link: the CAN driver interface, the in-process test bus and (on Linux) SocketCAN.
set(ANV_LINK_SOURCES src/link.cpp src/test_bus.cpp)
if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
  list(APPEND ANV_LINK_SOURCES src/socketcan_link.cpp)
endif()
add_library(anv_link STATIC ${ANV_LINK_SOURCES})
target_include_directories(anv_link PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_link_libraries(anv_link PUBLIC Threads::Threads)

# anv_proto: schema.json loader, 29-bit IDs, message codec, ISO-TP.
add_library(anv_proto STATIC src/schema.cpp src/codec.cpp src/isotp.cpp)
target_link_libraries(anv_proto PUBLIC anv_link)
# Tools built from the tree find the schema without being told; installed ones use $ANV_SCHEMA_JSON.
target_compile_definitions(anv_proto PRIVATE ANV_SCHEMA_JSON_DEFAULT="${ANV_SCHEMA_JSON}")

# anv_host: node discovery, ID assignment, leader election, and the clients for settings,
# rates, commands, actions, arm/disarm and E-stop.
add_library(anv_host STATIC src/host/election.cpp src/host/registry.cpp src/host/allocator.cpp src/host/host.cpp)
target_link_libraries(anv_host PUBLIC anv_proto)

foreach(lib anv_link anv_proto anv_host)
  target_compile_options(${lib} PRIVATE ${ANV_WARNINGS})
  if(ANV_WERROR)
    target_compile_options(${lib} PRIVATE -Werror)
  endif()
  set_target_properties(${lib} PROPERTIES POSITION_INDEPENDENT_CODE ON)
endforeach()

if(ANV_CAN_TESTS)
  enable_testing()
  add_subdirectory(tests)
endif()

if(ANV_CAN_PYTHON)
  find_package(Python 3.10 COMPONENTS Interpreter Development.Module REQUIRED)
  find_package(pybind11 CONFIG REQUIRED)
  pybind11_add_module(_anv_can MODULE python/bindings.cpp python/host_bindings.cpp)
  target_link_libraries(_anv_can PRIVATE anv_host)
  target_compile_options(_anv_can PRIVATE -Wall -Wextra)
  install(TARGETS _anv_can DESTINATION anv_can)
  # The wheel carries the schema it was built with, so it works away from the source tree.
  install(FILES ${ANV_SCHEMA_JSON} DESTINATION anv_can)
endif()
