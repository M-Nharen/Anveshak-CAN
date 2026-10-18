/* anv_link backend for the ESP32 / ESP32-S3 TWAI controller, over ESP-IDF 5.5's esp_twai "node"
 * driver (also what Arduino-ESP32 3.3+ ships). 29-bit extended data frames only: standard and
 * remote frames are dropped in the receive interrupt.
 *
 *   static anv_twai_t twai;
 *   static anv_link_t link;
 *   anv_twai_setup(&twai, TX_GPIO, RX_GPIO);
 *   anv_link_init(&link, anv_twai_ops(), &twai);
 *   anv_link_start(&link, &cfg);
 */
#ifndef ANV_LINK_TWAI_H
#define ANV_LINK_TWAI_H

#include "anv_link.h"

#include "esp_twai.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"

#ifdef __cplusplus
extern "C" {
#endif

#define ANV_TWAI_TX_SLOTS 8u
#define ANV_TWAI_RX_QUEUE_LEN 64u

typedef struct {
    int tx_gpio;
    int rx_gpio;
    twai_node_handle_t node;
    QueueHandle_t rx_queue;

    /* The driver keeps a pointer to each queued frame until it is sent, so every in-flight frame
     * owns one of these slots. TWAI sends strictly in order, so a ring is enough. */
    twai_frame_t tx_frames[ANV_TWAI_TX_SLOTS];
    uint8_t tx_data[ANV_TWAI_TX_SLOTS][ANV_LINK_MAX_DLC];
    uint8_t tx_head;
    volatile uint8_t tx_in_flight;
    portMUX_TYPE lock; /* tx_in_flight is changed from the caller's task and the TX-done ISR */

    volatile uint32_t tx_ok;       /* ACKed frames, bumped from the ISR (e.g. for an activity LED) */
    volatile uint32_t tx_failed;
    volatile uint32_t rx_overflow; /* RX queue was full */
    bool was_bus_off;

    /* Bus-off recovery rebuilds the controller from scratch (see twai_recover), so the backend
     * keeps what it was started with. */
    uint32_t bitrate;
    anv_link_filter_t filters[ANV_LINK_MAX_FILTERS];
    uint8_t n_filters;
    TickType_t bus_off_since;
    uint32_t restarts; /* times the controller was rebuilt after bus-off */
} anv_twai_t;

/* How long a node stays bus-off before the controller is rebuilt. anv_link asks for recovery as
 * soon as it sees bus-off and then every recover_retry_ms (1 s by default), so in practice a
 * node comes back about a second after going bus-off: long enough that a node with a real fault
 * doesn't flood the bus, short enough that a node that only lost its partners rejoins quickly. */
#define ANV_TWAI_RESTART_HOLDOFF_MS 200u

void anv_twai_setup(anv_twai_t *twai, int tx_gpio, int rx_gpio);
const anv_link_ops_t *anv_twai_ops(void);

/* For a serial console: raw TWAI status and the cumulative bus error count. */
bool anv_twai_get_info(anv_twai_t *twai, twai_node_status_t *status, twai_node_record_t *record);

#ifdef __cplusplus
}
#endif

#endif /* ANV_LINK_TWAI_H */
