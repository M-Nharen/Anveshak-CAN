# One file, two builds: an ESP-IDF component (idf.py, or EXTRA_COMPONENT_DIRS), or a plain static
# library for the Linux tests (firmware/CMakeLists.txt). Arduino and PlatformIO use library.*.
if(ESP_PLATFORM)
  idf_component_register(
    SRCS src/anv_link.c src/anv_link_loopback.c src/anv_link_twai_filter.c src/anv_link_twai.c
    INCLUDE_DIRS src
    REQUIRES esp_driver_twai freertos esp_hw_support)
  return()
endif()

add_library(anv_link STATIC
  src/anv_link.c
  src/anv_link_loopback.c
  src/anv_link_twai_filter.c
)
target_include_directories(anv_link PUBLIC src)
