# REQUIRES: twai and anveshak_can_core give the public API types main.c uses (acc_can_twai.h,
# anv_node.h, rover.h/ids.h via anveshak_can_core's public include dirs); driver is listed
# directly too since board_config.h (this component's own header) includes "driver/gpio.h" --
# relying on twai's own public requires on "driver" to propagate it here would work, but a
# component should declare what its own headers need rather than lean on a dependency's
# transitive requires.
# PRIV_REQUIRES: esp_timer is used only inside main.c's implementation (esp_timer_get_time()
# in board_now_ms()), never in a header this component exposes.
# freertos is in every component's implicit common requirements (ESP-IDF's requires_common),
# so it needs no entry here despite main.c including freertos/FreeRTOS.h and freertos/task.h.
idf_component_register(
  SRCS "main.c"
  INCLUDE_DIRS "."
  REQUIRES twai anveshak_can_core driver
  PRIV_REQUIRES esp_timer
)
