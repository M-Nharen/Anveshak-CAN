# Tests are plain executables registered with ctest; no test framework to install.
function(anv_add_test name)
  add_executable(${name} ${name}.cpp)
  target_link_libraries(${name} PRIVATE anv_gateway)
  target_compile_options(${name} PRIVATE -Wall -Wextra)
  if(ANV_WERROR)
    target_compile_options(${name} PRIVATE -Werror)
  endif()
  add_test(NAME ${name} COMMAND ${name})
  set_tests_properties(${name} PROPERTIES TIMEOUT 120)
endfunction()

anv_add_test(test_decoder)
anv_add_test(test_config)
anv_add_test(test_leadership)
anv_add_test(test_registry)
anv_add_test(test_gateway)
anv_add_test(test_bridge)
anv_add_test(test_allocator)

# End-to-end: the real executable against the Python simulator on vcan0. It skips itself (exit
# code 77) wherever vcan, python-can or cantools is missing, e.g. in a container without CAN.
if(TARGET anv_can_gateway)
  add_test(NAME vcan_smoke
    COMMAND ${CMAKE_CURRENT_SOURCE_DIR}/vcan_smoke.sh $<TARGET_FILE:anv_can_gateway>)
  set_tests_properties(vcan_smoke PROPERTIES SKIP_RETURN_CODE 77 TIMEOUT 120)
endif()
