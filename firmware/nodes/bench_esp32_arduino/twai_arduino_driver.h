/* acc_can_driver_t backend over ESP-IDF 5.5's esp_twai "node" driver (Arduino-ESP32 core 3.3+),
 * so the shared anveshak_can_core runs unchanged on the bench ESP32s. */
#ifndef TWAI_ARDUINO_DRIVER_H
#define TWAI_ARDUINO_DRIVER_H

#include <stdbool.h>
#include <stdint.h>

#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"

#include "esp_twai.h"
#include "src/anv/acc_can.h"

#ifdef __cplusplus
extern "C" {
#endif

#define TWAI_ARD_TX_SLOTS 8u
#define TWAI_ARD_RX_QUEUE_LEN 64u

typedef struct {
    int tx_gpio;
    int rx_gpio;
    twai_node_handle_t node;
    QueueHandle_t rx_queue;

    /* The driver keeps a pointer to each queued frame until it is sent, so every in-flight frame
     * owns one of these slots. TWAI sends strictly in order, so a ring is enough. */
    twai_frame_t tx_frames[TWAI_ARD_TX_SLOTS];
    uint8_t tx_data[TWAI_ARD_TX_SLOTS][8];
    uint8_t tx_head;
    volatile uint8_t tx_in_flight;
    portMUX_TYPE lock; /* tx_in_flight is changed from the loop task and the TX-done ISR */

    volatile uint32_t tx_ok_count; /* bumped from the ISR for every ACKed frame (LED flash) */
    volatile uint32_t rx_dropped;  /* RX queue was full */
    acc_can_error_state_t error_state;
} twai_ard_ctx_t;

acc_can_driver_t twai_ard_make_driver(twai_ard_ctx_t *ctx, int tx_gpio, int rx_gpio);

/* For the serial console: raw TWAI status and the cumulative bus error count. */
bool twai_ard_get_info(twai_ard_ctx_t *ctx, twai_node_status_t *status, twai_node_record_t *record);

#ifdef __cplusplus
}
#endif

#endif /* TWAI_ARDUINO_DRIVER_H */
