# ESP-IDF project that builds anv_link, anv_proto and anv_node as components, straight from their
# folders.
# CI: idf.py set-target esp32 (or esp32s3) && idf.py build
cmake_minimum_required(VERSION 3.16)
set(EXTRA_COMPONENT_DIRS
  ${CMAKE_CURRENT_LIST_DIR}/../../anv_link
  ${CMAKE_CURRENT_LIST_DIR}/../../anv_proto
  ${CMAKE_CURRENT_LIST_DIR}/../../anv_node)
include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(anv_lib_idf_check)
