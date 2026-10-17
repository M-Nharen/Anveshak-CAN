set(ANV_LIB_TESTS
  test_ids
  test_link
  test_isotp
)

foreach(test_name ${ANV_LIB_TESTS})
  add_executable(${test_name} ${test_name}.c)
  target_link_libraries(${test_name} PRIVATE anv_proto anv_link)
  # minitest.h is shared with the core tests.
  target_include_directories(${test_name} PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../../tests)
  target_compile_options(${test_name} PRIVATE -Werror)
  add_test(NAME lib_${test_name} COMMAND ${test_name})
endforeach()

# The libraries themselves are held to the same bar.
target_compile_options(anv_link PRIVATE -Werror -Wpedantic)
target_compile_options(anv_proto PRIVATE -Werror -Wpedantic)
