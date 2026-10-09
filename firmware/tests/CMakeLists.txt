set(ANV_CORE_TESTS
  test_master_arming
  test_command_watchdog
  test_sync_apply
  test_estop
  test_params
  test_busoff
  test_diag_events
  test_blackbox
  test_debug_levels
  test_rates
)

foreach(test_name ${ANV_CORE_TESTS})
  add_executable(${test_name} ${test_name}.c)
  target_link_libraries(${test_name} PRIVATE anveshak_can_core acc_can_loopback)
  target_include_directories(${test_name} PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
  add_test(NAME ${test_name} COMMAND ${test_name})
endforeach()
