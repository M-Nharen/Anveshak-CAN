#include "anv_txqueue.h"

void anv_txqueue_init(anv_txqueue_t *queue, acc_can_frame_t *storage, size_t per_priority_capacity)
{
    for (size_t i = 0; i < (size_t)ACC_CAN_PRIO_COUNT; i++) {
        anv_frame_ring_init(&queue->rings[i], storage + (i * per_priority_capacity), per_priority_capacity);
    }
}

bool anv_txqueue_push(anv_txqueue_t *queue, acc_can_priority_t prio, const acc_can_frame_t *frame)
{
    if ((size_t)prio >= (size_t)ACC_CAN_PRIO_COUNT) {
        return false;
    }
    return anv_frame_ring_push(&queue->rings[prio], frame);
}

bool anv_txqueue_is_empty(const anv_txqueue_t *queue)
{
    for (size_t i = 0; i < (size_t)ACC_CAN_PRIO_COUNT; i++) {
        if (!anv_frame_ring_is_empty(&queue->rings[i])) {
            return false;
        }
    }
    return true;
}

size_t anv_txqueue_drain(anv_txqueue_t *queue, const acc_can_driver_t *driver)
{
    size_t sent = 0;
    for (size_t i = 0; i < (size_t)ACC_CAN_PRIO_COUNT; i++) {
        acc_can_frame_t frame;
        while (!anv_frame_ring_is_empty(&queue->rings[i])) {
            /* Peek without popping so a refused frame is not lost. */
            acc_can_frame_t *head = &queue->rings[i].storage[queue->rings[i].head];
            frame = *head;
            int status = driver->send(driver->ctx, &frame, (acc_can_priority_t)i);
            if (status != ACC_CAN_OK) {
                return sent;
            }
            (void)anv_frame_ring_pop(&queue->rings[i], &frame);
            sent++;
        }
    }
    return sent;
}
