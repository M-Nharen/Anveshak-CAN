add_library(anveshak_can_core STATIC
  src/anv_ring.c
  src/anv_txqueue.c
  src/anv_state.c
  src/anv_master.c
  src/anv_cmdwatch.c
  src/anv_syncbuf.c
  src/anv_params.c
  src/anv_service.c
  src/anv_nmt.c
  src/anv_estop.c
  src/anv_heartbeat.c
  src/anv_debug.c
  src/anv_diag.c
  src/anv_node.c
  ${ROVER_GENERATED_DIR}/rover.c
)

target_include_directories(anveshak_can_core PUBLIC
  include
  ${ROVER_GENERATED_DIR}
)
