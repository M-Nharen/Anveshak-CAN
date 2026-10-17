set(ANV_LIB_TESTS
  test_ids
  test_link
  test_isotp
)
# anv_node tests run real generated node types from schema/generated/c.
set(ANV_SCHEMA_C_DIR ${CMAKE_CURRENT_LIST_DIR}/../../../schema/generated/c)
set(ANV_NODE_TESTS
  test_node_id
  test_node_settings
  test_node_safety
  test_ota
  test_diag
)

foreach(test_name ${ANV_LIB_TESTS})
  add_executable(${test_name} ${test_name}.c)
  target_link_libraries(${test_name} PRIVATE anv_proto anv_link)
  # minitest.h is shared with the core tests.
  target_include_directories(${test_name} PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../../tests)
  target_compile_options(${test_name} PRIVATE -Werror)
  add_test(NAME lib_${test_name} COMMAND ${test_name})
endforeach()

foreach(test_name ${ANV_NODE_TESTS})
  add_executable(${test_name} ${test_name}.c node_harness.c
    ${ANV_SCHEMA_C_DIR}/anv_motor_controller.c ${ANV_SCHEMA_C_DIR}/anv_esp32_bench.c)
  target_link_libraries(${test_name} PRIVATE anv_node)
  target_include_directories(${test_name} PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../../tests)
  target_compile_options(${test_name} PRIVATE -Werror)
  add_test(NAME lib_${test_name} COMMAND ${test_name})
endforeach()

# The libraries themselves are held to the same bar.
target_compile_options(anv_link PRIVATE -Werror -Wpedantic)
target_compile_options(anv_proto PRIVATE -Werror -Wpedantic)
target_compile_options(anv_node PRIVATE -Werror -Wpedantic)
