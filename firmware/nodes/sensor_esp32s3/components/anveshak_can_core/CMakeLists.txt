# Wraps the shared anveshak_can_core sources (firmware/core) and generated DBC code
# (firmware/generated) as an ESP-IDF component, without copying or forking either -- they stay
# owned by firmware/core/CMakeLists.txt for the plain-CMake (host/ARM) builds.
set(ANV_CORE_DIR "${CMAKE_CURRENT_LIST_DIR}/../../../../core")
set(ANV_GENERATED_DIR "${CMAKE_CURRENT_LIST_DIR}/../../../../generated")

idf_component_register(
  SRCS
    "${ANV_CORE_DIR}/src/anv_ring.c"
    "${ANV_CORE_DIR}/src/anv_txqueue.c"
    "${ANV_CORE_DIR}/src/anv_state.c"
    "${ANV_CORE_DIR}/src/anv_master.c"
    "${ANV_CORE_DIR}/src/anv_cmdwatch.c"
    "${ANV_CORE_DIR}/src/anv_syncbuf.c"
    "${ANV_CORE_DIR}/src/anv_params.c"
    "${ANV_CORE_DIR}/src/anv_service.c"
    "${ANV_CORE_DIR}/src/anv_nmt.c"
    "${ANV_CORE_DIR}/src/anv_estop.c"
    "${ANV_CORE_DIR}/src/anv_heartbeat.c"
    "${ANV_CORE_DIR}/src/anv_node.c"
    "${ANV_GENERATED_DIR}/rover.c"
  INCLUDE_DIRS
    "${ANV_CORE_DIR}/include"
    "${ANV_GENERATED_DIR}"
)
