#include "anv_ring.h"

void anv_frame_ring_init(anv_frame_ring_t *ring, acc_can_frame_t *storage, size_t capacity)
{
    ring->storage = storage;
    ring->capacity = capacity;
    ring->head = 0;
    ring->tail = 0;
    ring->count = 0;
}

bool anv_frame_ring_push(anv_frame_ring_t *ring, const acc_can_frame_t *frame)
{
    if (anv_frame_ring_is_full(ring)) {
        return false;
    }
    ring->storage[ring->tail] = *frame;
    ring->tail = (ring->tail + 1u) % ring->capacity;
    ring->count++;
    return true;
}

bool anv_frame_ring_pop(anv_frame_ring_t *ring, acc_can_frame_t *frame)
{
    if (anv_frame_ring_is_empty(ring)) {
        return false;
    }
    *frame = ring->storage[ring->head];
    ring->head = (ring->head + 1u) % ring->capacity;
    ring->count--;
    return true;
}

bool anv_frame_ring_is_empty(const anv_frame_ring_t *ring)
{
    return ring->count == 0u;
}

bool anv_frame_ring_is_full(const anv_frame_ring_t *ring)
{
    return ring->count == ring->capacity;
}

size_t anv_frame_ring_count(const anv_frame_ring_t *ring)
{
    return ring->count;
}
