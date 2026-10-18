# One file, two builds: an ESP-IDF component, or a plain static library for the Linux tests.
# The generated protocol code is compiled in place from schema/generated/c (anv_node_protocol.c).
set(ANV_SCHEMA_C_DIR ${CMAKE_CURRENT_LIST_DIR}/../../../schema/generated/c)

if(ESP_PLATFORM)
  idf_component_register(
    SRCS src/anv_node.c src/anv_node_settings.c src/anv_node_memstore.c src/anv_node_protocol.c
         src/anv_node_esp32.c src/anv_node_diag.c src/anv_node_ota.c src/anv_node_ota_esp32.c
    INCLUDE_DIRS src ${ANV_SCHEMA_C_DIR}
    REQUIRES anv_link anv_proto nvs_flash esp_system esp_hw_support app_update)
  return()
endif()

add_library(anv_node STATIC
  src/anv_node.c
  src/anv_node_settings.c
  src/anv_node_memstore.c
  src/anv_node_protocol.c
  src/anv_node_esp32.c
  src/anv_node_diag.c
  src/anv_node_ota.c
  src/anv_node_ota_esp32.c
)
target_include_directories(anv_node PUBLIC src ${ANV_SCHEMA_C_DIR})
target_link_libraries(anv_node PUBLIC anv_proto anv_link)
