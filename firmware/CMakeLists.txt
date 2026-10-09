cmake_minimum_required(VERSION 3.16)
project(anveshak_can_firmware C)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS OFF)

if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Debug)
endif()

add_compile_options(-Wall -Wextra)

set(ROVER_GENERATED_DIR ${CMAKE_CURRENT_SOURCE_DIR}/generated)

add_subdirectory(core)
add_subdirectory(drivers/loopback)

# Unit tests run only on the host build; they cannot run on a cross-compiled Cortex-M4 target.
if(NOT CMAKE_CROSSCOMPILING)
  enable_testing()
  add_subdirectory(tests)
endif()
