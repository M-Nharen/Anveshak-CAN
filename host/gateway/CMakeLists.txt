cmake_minimum_required(VERSION 3.16)
project(anv_gateway VERSION 0.1.0 LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_C_STANDARD 11)
if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
  set(CMAKE_BUILD_TYPE RelWithDebInfo)
endif()

option(ANV_WERROR "Treat warnings in gateway sources as errors" OFF)
option(ANV_GATEWAY_TESTS "Build the gateway tests" ON)

find_package(Threads REQUIRED)

# Generated CAN code (cantools, from defs/rover.dbc). Compiled as C, warnings left alone: it is
# regenerated, never edited.
set(ANV_GENERATED_DIR ${CMAKE_CURRENT_SOURCE_DIR}/../../firmware/generated)
add_library(anv_rover_generated STATIC ${ANV_GENERATED_DIR}/rover.c)
target_include_directories(anv_rover_generated PUBLIC ${ANV_GENERATED_DIR})
set_target_properties(anv_rover_generated PROPERTIES POSITION_INDEPENDENT_CODE ON)

set(ANV_GATEWAY_SOURCES
  src/fake_bus.cpp
  src/decoder.cpp
  src/encoder.cpp
  src/node_registry.cpp
  src/leadership.cpp
  src/config.cpp
  src/status.cpp
  src/gateway.cpp
  src/events.cpp
  src/bridge.cpp
  src/id_allocator.cpp)
if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
  list(APPEND ANV_GATEWAY_SOURCES src/socketcan_bus.cpp)
endif()

add_library(anv_gateway STATIC ${ANV_GATEWAY_SOURCES})
target_include_directories(anv_gateway PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_link_libraries(anv_gateway PUBLIC anv_rover_generated Threads::Threads)
target_compile_options(anv_gateway PRIVATE -Wall -Wextra)
if(ANV_WERROR)
  target_compile_options(anv_gateway PRIVATE -Werror)
endif()
set_target_properties(anv_gateway PROPERTIES POSITION_INDEPENDENT_CODE ON)

if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
  add_executable(anv_can_gateway src/main.cpp)
  target_link_libraries(anv_can_gateway PRIVATE anv_gateway)
  target_compile_options(anv_can_gateway PRIVATE -Wall -Wextra)
  if(ANV_WERROR)
    target_compile_options(anv_can_gateway PRIVATE -Werror)
  endif()
  install(TARGETS anv_can_gateway RUNTIME DESTINATION bin)
endif()

if(ANV_GATEWAY_TESTS)
  enable_testing()
  add_subdirectory(tests)
endif()
