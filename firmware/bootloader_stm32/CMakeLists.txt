# STM32 CAN bootloader (docs/architecture.md, "Firmware updates over CAN"); see README.md.
#
# The bootloader logic (src/) is hardware-free and also builds for Linux, where tests/ drives
# it through the loopback CAN driver and a fake flash. The STM32 images (port/) build when
# cross-compiling with the same HAL/CMSIS variables as the drive node (see README.md).

set(ANV_BL_PORTABLE_SOURCES
  ${CMAKE_CURRENT_SOURCE_DIR}/src/anv_bl.c
  ${CMAKE_CURRENT_SOURCE_DIR}/src/anv_bl_cmd.c
  ${CMAKE_CURRENT_SOURCE_DIR}/src/anv_bl_flash.c
  ${CMAKE_CURRENT_SOURCE_DIR}/src/anv_bl_image.c
  ${CMAKE_CURRENT_SOURCE_DIR}/src/anv_isotp_port.c
  ${CMAKE_CURRENT_SOURCE_DIR}/third_party/isotp-c/isotp.c
)
set(ANV_BL_INCLUDE_DIRS
  ${CMAKE_CURRENT_SOURCE_DIR}/include
  ${CMAKE_CURRENT_SOURCE_DIR}/third_party/isotp-c
)
# Per-link driver context, padded frames (CAN 2.0 receivers expect DLC 8), and no snprintf.
set(ANV_BL_ISOTP_DEFINES ISO_TP_USER_SEND_CAN_ARG ISO_TP_FRAME_PADDING ISO_TP_NO_FORMATTED_ERRORS)

# Third-party code: compiled as-is, its warnings are not ours to fix.
set_source_files_properties(${CMAKE_CURRENT_SOURCE_DIR}/third_party/isotp-c/isotp.c PROPERTIES COMPILE_OPTIONS "-w")

if(NOT CMAKE_CROSSCOMPILING)
  add_library(anv_bootloader_logic STATIC ${ANV_BL_PORTABLE_SOURCES})
  target_include_directories(anv_bootloader_logic PUBLIC ${ANV_BL_INCLUDE_DIRS})
  target_compile_definitions(anv_bootloader_logic PUBLIC ${ANV_BL_ISOTP_DEFINES})
  target_link_libraries(anv_bootloader_logic PUBLIC anveshak_can_core)

  add_library(anv_bl_fake_flash STATIC tests/fake_flash.c)
  target_link_libraries(anv_bl_fake_flash PUBLIC anv_bootloader_logic)

  foreach(test_name test_bl_cmd test_bl_boot)
    add_executable(${test_name} tests/${test_name}.c)
    target_link_libraries(${test_name} PRIVATE anv_bl_fake_flash acc_can_loopback)
    target_include_directories(${test_name} PRIVATE tests ${CMAKE_SOURCE_DIR}/tests)
    add_test(NAME ${test_name} COMMAND ${test_name})
  endforeach()

  # The real bootloader behind stdin/stdout, for anv-flash's interop test
  # (host/tools/anv_flash/tests/test_real_bootloader.py, which finds it via ANV_BL_STDIO_NODE).
  add_executable(bl_stdio_node tests/bl_stdio_node.c)
  target_link_libraries(bl_stdio_node PRIVATE anv_bl_fake_flash acc_can_loopback)
  target_include_directories(bl_stdio_node PRIVATE tests)

  # Application half of the hand-over: ENTER_BOOTLOADER in the core library.
  add_executable(test_enter_bootloader tests/test_enter_bootloader.c)
  target_link_libraries(test_enter_bootloader PRIVATE anveshak_can_core acc_can_loopback)
  target_include_directories(test_enter_bootloader PRIVATE ${CMAKE_SOURCE_DIR}/tests)
  add_test(NAME test_enter_bootloader COMMAND test_enter_bootloader)
elseif(ANV_BUILD_DRIVE_STM32_F446 OR ANV_BUILD_DRIVE_STM32_G431)
  include(${CMAKE_CURRENT_SOURCE_DIR}/cmake/stm32_images.cmake)
endif()
