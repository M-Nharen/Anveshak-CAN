# v2 libraries, Linux build: the libraries plus their unit tests (loopback driver, no hardware).
add_subdirectory(anv_link)
add_subdirectory(anv_proto)

if(NOT CMAKE_CROSSCOMPILING)
  add_subdirectory(tests)
endif()
