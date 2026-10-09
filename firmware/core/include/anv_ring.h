/* Fixed-capacity ring buffer of CAN frames. No dynamic allocation: the caller provides the
 * backing array. Used for the receive ring (ISR-fed on real hardware) and for each priority
 * bucket of the software transmit queue. */
#ifndef ANV_RING_H
#define ANV_RING_H

#include <stdbool.h>
#include <stddef.h>

#include "acc_can.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    acc_can_frame_t *storage;
    size_t capacity;
    size_t head;
    size_t tail;
    size_t count;
} anv_frame_ring_t;

void anv_frame_ring_init(anv_frame_ring_t *ring, acc_can_frame_t *storage, size_t capacity);
bool anv_frame_ring_push(anv_frame_ring_t *ring, const acc_can_frame_t *frame);
bool anv_frame_ring_pop(anv_frame_ring_t *ring, acc_can_frame_t *frame);
bool anv_frame_ring_is_empty(const anv_frame_ring_t *ring);
bool anv_frame_ring_is_full(const anv_frame_ring_t *ring);
size_t anv_frame_ring_count(const anv_frame_ring_t *ring);

#ifdef __cplusplus
}
#endif

#endif /* ANV_RING_H */
