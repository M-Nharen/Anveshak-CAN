/* TWAI driver (ESP32-S3, ESP-IDF's legacy `driver/twai.h` API) implementing acc_can_driver_t
 * (acc_can.h), so anveshak_can_core runs on the ESP32-S3 the same way it does on STM32.
 *
 * docs/architecture.md, "Controller setup per device" (ESP32-S3 nodes) and "Hardware filters
 * on nodes":
 *   - 500 kbps: prescaler 8, seg1 15, seg2 4, SJW 3 (20 TQ per bit, 80% sample point).
 *   - Dual filter mode with the same node-addressed / class-0 two-filter recipe as the STM32
 *     drivers. TWAI's acceptance mask is 1 = don't care (the opposite of bxCAN/FDCAN), which
 *     this driver accounts for internally so the caller's acc_can_filter_t keeps its usual
 *     1 = must match meaning.
 *   - RX is read by a dedicated FreeRTOS task pinned to core 1 (so Wi-Fi/Bluetooth on core 0
 *     never delays CAN) and fed into the core's ring buffer; driver.receive() only pops from
 *     that ring, matching the non-blocking semantics the STM32 drivers' ISRs provide.
 *   - Bus-off does not clear by itself: the core's generic recovery path calls
 *     start_recovery(), which drives TWAI's initiate-recovery / restart sequence.
 */
#ifndef ACC_CAN_TWAI_H
#define ACC_CAN_TWAI_H

#include "acc_can.h"
#include "anv_ring.h"

#include "driver/gpio.h"
#include "driver/twai.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#ifdef __cplusplus
extern "C" {
#endif

#define ACC_CAN_TWAI_RX_CAPACITY 32u
#define ACC_CAN_TWAI_RX_TASK_STACK_WORDS 3072u
#define ACC_CAN_TWAI_RX_TASK_PRIORITY (configMAX_PRIORITIES - 2)
#define ACC_CAN_TWAI_RX_TASK_CORE 1 /* core 0 is left for Wi-Fi/Bluetooth */

typedef struct {
    gpio_num_t tx_io;
    gpio_num_t rx_io;
    anv_frame_ring_t rx_ring;
    acc_can_frame_t rx_storage[ACC_CAN_TWAI_RX_CAPACITY];
    acc_can_error_state_t error_state;
    TaskHandle_t rx_task;
} acc_can_twai_ctx_t;

/* Builds the acc_can_driver_t vtable backed by `ctx`. `tx_io`/`rx_io` are this board's TWAI
 * GPIO pins (board_config.h). init() installs the driver with the 500 kbps timing above,
 * starts it, and spawns the RX task described above. */
acc_can_driver_t acc_can_twai_make_driver(acc_can_twai_ctx_t *ctx, gpio_num_t tx_io, gpio_num_t rx_io);

#ifdef __cplusplus
}
#endif

#endif /* ACC_CAN_TWAI_H */
