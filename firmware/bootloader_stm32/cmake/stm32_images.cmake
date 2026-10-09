# Bootloader images for the boards the drive node builds for (same HAL/CMSIS variables and
# the same board_config.h, so the bootloader's CAN and node-ID pins always match the
# application's). Everything is compiled here at -Os with section GC, rather than linking the
# Debug-built core/driver libraries, to stay inside the 32 KB the linker script allows.
include(${CMAKE_CURRENT_LIST_DIR}/anv_stm32.cmake)

set(ANV_BL_NODE_TYPE 1 CACHE STRING "NODE_HB node type the bootloader accepts images for (1 = drive)")

function(anv_bootloader_image board)
  anv_stm32_family(${board})
  string(TOLOWER ${board} b)
  set(t anv_bootloader_${b})

  add_executable(${t}
    ${ANV_BL_PORTABLE_SOURCES}
    ${ANV_BL_ROOT_DIR}/port/stm32/anv_bl_main.c
    ${ANV_BL_ROOT_DIR}/port/stm32/anv_bl_stm32.c
    ${ANV_BL_ROOT_DIR}/port/stm32/anv_bl_stay_flag.c
    ${ANV_BL_ROOT_DIR}/port/${b}/anv_bl_board_${b}.c
    ${CMAKE_SOURCE_DIR}/core/src/anv_ring.c
    ${CMAKE_SOURCE_DIR}/core/src/anv_txqueue.c
    ${CMAKE_SOURCE_DIR}/core/src/anv_heartbeat.c
    ${ROVER_GENERATED_DIR}/rover.c
    ${ANV_FAM_CAN_SOURCE}
    ${ANV_FAM_STARTUP}
    ${ANV_FAM_SYSTEM}
  )
  target_include_directories(${t} PRIVATE
    ${ANV_BL_INCLUDE_DIRS}
    ${ANV_BL_ROOT_DIR}/port/stm32
    ${CMAKE_SOURCE_DIR}/core/include
    ${ROVER_GENERATED_DIR}
    ${ANV_FAM_CAN_INCLUDE}
    ${CMAKE_SOURCE_DIR}/nodes/drive_stm32/include
    ${CMAKE_SOURCE_DIR}/nodes/drive_stm32/boards/${b}
  )
  target_compile_definitions(${t} PRIVATE
    ${ANV_BL_ISOTP_DEFINES} NDEBUG
    ANV_BL_FAMILY_${ANV_FAM} DRIVE_BOARD_${board} ANV_BL_NODE_TYPE=${ANV_BL_NODE_TYPE}u
    ${ANV_FAM_CAN_DEFINES}
  )
  target_compile_options(${t} PRIVATE ${ANV_STM32_SIZE_FLAGS})
  target_link_libraries(${t} PRIVATE ${ANV_FAM_HAL_LIB})
  anv_stm32_link(${t} ${ANV_BL_ROOT_DIR}/port/${b}/bootloader_${b}.ld)
  add_custom_command(TARGET ${t} POST_BUILD
    COMMAND ${CMAKE_OBJCOPY} -O binary $<TARGET_FILE:${t}> $<TARGET_FILE_DIR:${t}>/${t}.bin
    COMMENT "${t}.bin (flash at 0x08000000 over SWD, then write-protect: see bootloader_stm32/README.md)")
endfunction()

if(ANV_BUILD_DRIVE_STM32_F446)
  anv_bootloader_image(F446)
endif()
if(ANV_BUILD_DRIVE_STM32_G431)
  anv_bootloader_image(G431)
endif()
