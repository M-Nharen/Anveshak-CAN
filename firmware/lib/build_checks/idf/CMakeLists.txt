# ESP-IDF project that builds anv_link and anv_proto as components, straight from their folders.
# CI: idf.py set-target esp32 (or esp32s3) && idf.py build
cmake_minimum_required(VERSION 3.16)
set(EXTRA_COMPONENT_DIRS
  ${CMAKE_CURRENT_LIST_DIR}/../../anv_link
  ${CMAKE_CURRENT_LIST_DIR}/../../anv_proto)
include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(anv_lib_idf_check)
