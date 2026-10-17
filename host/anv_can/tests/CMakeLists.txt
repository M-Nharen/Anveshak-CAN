# Plain executables registered with ctest; no test framework to install.

# The generated C the firmware compiles, so test_proto can compare against it byte for byte.
file(GLOB ANV_SCHEMA_C_SOURCES ${ANV_SCHEMA_C}/*.c)
add_library(anv_schema_c STATIC ${ANV_SCHEMA_C_SOURCES})
target_include_directories(anv_schema_c PUBLIC ${ANV_SCHEMA_C})

function(anv_can_test name)
  add_executable(${name} ${name}.cpp)
  target_link_libraries(${name} PRIVATE anv_proto anv_schema_c)
  target_compile_options(${name} PRIVATE -Wall -Wextra)
  if(ANV_WERROR)
    target_compile_options(${name} PRIVATE -Werror)
  endif()
  add_test(NAME ${name} COMMAND ${name})
  set_tests_properties(${name} PROPERTIES TIMEOUT 60 ENVIRONMENT "ANV_SCHEMA_JSON=${ANV_SCHEMA_JSON}")
endfunction()

anv_can_test(test_link)
anv_can_test(test_proto)
anv_can_test(test_isotp)
