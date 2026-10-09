# drive_app: motor PID/ramp/mode logic and the anv_node_t wiring. No HAL dependency, so it
# builds (and is unit-tested) on the host the same way it cross-compiles for a real board.
add_library(drive_app STATIC
  app/drive_motor.c
  app/drive_app.c
)
target_include_directories(drive_app PUBLIC app include)
target_link_libraries(drive_app PUBLIC anveshak_can_core)

if(NOT CMAKE_CROSSCOMPILING)
  add_executable(test_drive_motor tests/test_drive_motor.c)
  target_link_libraries(test_drive_motor PRIVATE drive_app)
  target_include_directories(test_drive_motor PRIVATE ${CMAKE_SOURCE_DIR}/tests)
  add_test(NAME test_drive_motor COMMAND test_drive_motor)

  add_executable(test_drive_app tests/test_drive_app.c)
  target_link_libraries(test_drive_app PRIVATE drive_app acc_can_loopback)
  target_include_directories(test_drive_app PRIVATE ${CMAKE_SOURCE_DIR}/tests)
  add_test(NAME test_drive_app COMMAND test_drive_app)
endif()

# Board targets: each proves one CAN peripheral family compiles for one real part
# (CLAUDE.md "Done when": "Build for one F4 part ... and one G4 part ... to prove both
# drivers compile"). Built as static libraries, not linked executables: like
# firmware/cmake/arm-none-eabi.cmake, there is no startup code, vector table or linker script
# here (that is a real board project's job -- see README.md), so there is nothing to link
# against yet. Both are off unless the caller supplies the matching HAL/CMSIS headers; see
# README.md for the exact CMake invocation CI uses.
if(ANV_BUILD_DRIVE_STM32_F446)
  add_library(drive_stm32_f446 STATIC
    src/main_f446.c
    src/board_hw_f446.c
  )
  target_include_directories(drive_stm32_f446 PRIVATE boards/f446)
  target_compile_definitions(drive_stm32_f446 PRIVATE DRIVE_BOARD_F446)
  target_link_libraries(drive_stm32_f446 PRIVATE drive_app acc_can_bxcan)
endif()

if(ANV_BUILD_DRIVE_STM32_G431)
  add_library(drive_stm32_g431 STATIC
    src/main_g431.c
    src/board_hw_g431.c
  )
  target_include_directories(drive_stm32_g431 PRIVATE boards/g431)
  target_compile_definitions(drive_stm32_g431 PRIVATE DRIVE_BOARD_G431)
  target_link_libraries(drive_stm32_g431 PRIVATE drive_app acc_can_fdcan)
endif()
