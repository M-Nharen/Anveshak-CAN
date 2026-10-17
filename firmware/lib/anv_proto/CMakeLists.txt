# One file, two builds: an ESP-IDF component, or a plain static library for the Linux tests.
# The generated protocol header is used in place, from schema/generated/c.
set(ANV_SCHEMA_C_DIR ${CMAKE_CURRENT_LIST_DIR}/../../../schema/generated/c)

if(ESP_PLATFORM)
  idf_component_register(
    SRCS src/anv_proto.c src/anv_isotp.c
    INCLUDE_DIRS src ${ANV_SCHEMA_C_DIR}
    REQUIRES anv_link)
  return()
endif()

add_library(anv_proto STATIC
  src/anv_proto.c
  src/anv_isotp.c
)
target_include_directories(anv_proto PUBLIC src ${ANV_SCHEMA_C_DIR})
target_link_libraries(anv_proto PUBLIC anv_link)
