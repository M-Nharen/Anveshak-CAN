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

# Unit tests run only on the host build, never when cross-compiling.
# enable_testing() must come before any add_subdirectory() that calls add_test(), so it is
# called here, ahead of every subdirectory below.
if(NOT CMAKE_CROSSCOMPILING)
  enable_testing()
endif()

add_subdirectory(core)
add_subdirectory(drivers/loopback)
add_subdirectory(lib)


if(NOT CMAKE_CROSSCOMPILING)
  add_subdirectory(tests)
endif()
