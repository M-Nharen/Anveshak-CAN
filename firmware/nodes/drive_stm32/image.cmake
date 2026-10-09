# Flashable drive node images that run behind the CAN bootloader: the board library linked
# with ST's startup/system files and HAL at the application address (ld/drive_app_*.ld), then
# stamped with length, CRC32 and build ID (host/tools/anv_flash/anv_flash/image.py).
# Output: build-*/nodes/drive_stm32/drive_app_<board>.bin, ready for `anv-flash flash`.
include(${CMAKE_SOURCE_DIR}/bootloader_stm32/cmake/anv_stm32.cmake)

function(drive_app_image board vect_tab_offset)
  anv_stm32_family(${board})
  string(TOLOWER ${board} b)
  set(lib drive_stm32_${b})
  set(t drive_app_${b})

  # Header and bootloader hand-over (ENTER_BOOTLOADER sets the stay flag) for the board library.
  target_sources(${lib} PRIVATE src/app_image.c ${ANV_BL_ROOT_DIR}/port/stm32/anv_bl_stay_flag.c)
  target_include_directories(${lib} PRIVATE ${ANV_BL_ROOT_DIR}/include ${ANV_BL_ROOT_DIR}/port/stm32)
  target_compile_definitions(${lib} PRIVATE ANV_BL_FAMILY_${ANV_FAM})

  add_executable(${t} ${ANV_FAM_STARTUP} ${ANV_FAM_SYSTEM})
  # SystemInit() points VTOR at our vector table, just past the header slot.
  set_source_files_properties(${ANV_FAM_SYSTEM} PROPERTIES
    COMPILE_DEFINITIONS "USER_VECT_TAB_ADDRESS;VECT_TAB_OFFSET=${vect_tab_offset}")
  target_link_libraries(${t} PRIVATE ${lib} ${ANV_FAM_HAL_LIB})
  anv_stm32_link(${t} ${CMAKE_CURRENT_SOURCE_DIR}/ld/drive_app_${b}.ld)
  add_custom_command(TARGET ${t} POST_BUILD
    COMMAND ${CMAKE_OBJCOPY} -O binary $<TARGET_FILE:${t}> $<TARGET_FILE_DIR:${t}>/${t}_raw.bin
    COMMAND ${Python3_EXECUTABLE} ${ANV_IMAGE_TOOL} stamp $<TARGET_FILE_DIR:${t}>/${t}_raw.bin
            $<TARGET_FILE_DIR:${t}>/${t}.bin --git-dir ${ANV_REPO_ROOT}
    COMMENT "Stamping ${t}.bin")
endfunction()

if(ANV_BUILD_DRIVE_STM32_F446)
  drive_app_image(F446 0x10200U)
endif()
if(ANV_BUILD_DRIVE_STM32_G431)
  drive_app_image(G431 0x9200U)
endif()
