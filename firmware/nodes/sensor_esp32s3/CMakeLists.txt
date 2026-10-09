# ESP-IDF project for an ESP32-S3 node template (science/auxiliary, node IDs 16-19;
# docs/architecture.md "Identifier scheme"). Built with the official espressif/idf Docker
# image in CI (.github/workflows/ci.yml); not installable in this repo's other CI jobs, so it
# is not built there (CLAUDE.md: "if a tool can't be installed ... add a GitHub Actions
# workflow so CI builds and tests it").
#
# Pulls in anveshak_can_core (firmware/core, shared with every other node) and acc_can_twai
# (firmware/drivers/twai) as components via EXTRA_COMPONENT_DIRS, rather than copying either.
cmake_minimum_required(VERSION 3.16)

list(APPEND EXTRA_COMPONENT_DIRS
  "${CMAKE_CURRENT_LIST_DIR}/../../drivers/twai"
  "${CMAKE_CURRENT_LIST_DIR}/components/anveshak_can_core"
)

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(sensor_esp32s3)
