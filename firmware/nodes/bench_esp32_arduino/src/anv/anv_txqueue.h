/* Software transmit queue sorted by priority: E-stop, then feedback, then telemetry, then
 * service/bootloader traffic (acc_can_priority_t). No dynamic allocation. */
#ifndef ANV_TXQUEUE_H
#define ANV_TXQUEUE_H

#include <stdbool.h>
#include <stddef.h>

#include "acc_can.h"
#include "anv_ring.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    anv_frame_ring_t rings[ACC_CAN_PRIO_COUNT];
} anv_txqueue_t;

/* `storage` must point to an array of (ACC_CAN_PRIO_COUNT * per_priority_capacity) frames;
 * it is sliced evenly across the priority buckets. */
void anv_txqueue_init(anv_txqueue_t *queue, acc_can_frame_t *storage, size_t per_priority_capacity);

bool anv_txqueue_push(anv_txqueue_t *queue, acc_can_priority_t prio, const acc_can_frame_t *frame);
bool anv_txqueue_is_empty(const anv_txqueue_t *queue);

/* Send everything currently queued, highest priority first, by calling driver->send(). Stops
 * as soon as the driver refuses a frame (e.g. ACC_CAN_ERR_NOT_READY while bus-off); that frame
 * and everything behind it stay queued for the next call. Returns the number of frames sent. */
size_t anv_txqueue_drain(anv_txqueue_t *queue, const acc_can_driver_t *driver);

#ifdef __cplusplus
}
#endif

#endif /* ANV_TXQUEUE_H */
