#include "board_log_flash.h"

#include <string.h>

static uint32_t slot_count(const board_log_flash_t *f)
{
    return f->size / BOARD_LOG_SLOT_BYTES;
}

static const uint8_t *slot_ptr(const board_log_flash_t *f, uint32_t slot)
{
    return f->mem + (size_t)slot * BOARD_LOG_SLOT_BYTES;
}

static int slot_committed(const board_log_flash_t *f, uint32_t slot)
{
    const uint8_t *p = slot_ptr(f, slot);
    return !(p[0] == 0xFFu && p[1] == 0xFFu && p[2] == 0xFFu && p[3] == 0xFFu);
}

static int slot_blank(const board_log_flash_t *f, uint32_t slot)
{
    const uint8_t *p = slot_ptr(f, slot);
    for (uint32_t i = 0; i < BOARD_LOG_SLOT_BYTES; i++) {
        if (p[i] != 0xFFu) {
            return 0;
        }
    }
    return 1;
}

int board_log_flash_load(void *ctx, uint8_t *data, size_t len)
{
    const board_log_flash_t *f = (const board_log_flash_t *)ctx;
    if (len > BOARD_LOG_SLOT_BYTES) {
        return -1;
    }
    for (uint32_t i = slot_count(f); i > 0; i--) {
        if (slot_committed(f, i - 1u)) {
            memcpy(data, slot_ptr(f, i - 1u), len);
            return 0;
        }
    }
    return -1; /* nothing saved yet */
}

int board_log_flash_save(void *ctx, const uint8_t *data, size_t len)
{
    board_log_flash_t *f = (board_log_flash_t *)ctx;
    if (len > BOARD_LOG_SLOT_BYTES) {
        return -1;
    }

    /* The next slot after the newest committed one (a torn, non-blank slot is skipped). */
    uint32_t n = slot_count(f);
    uint32_t start = 0;
    for (uint32_t i = n; i > 0; i--) {
        if (slot_committed(f, i - 1u)) {
            start = i;
            break;
        }
    }
    uint32_t target = n;
    for (uint32_t i = start; i < n; i++) {
        if (slot_blank(f, i)) {
            target = i;
            break;
        }
    }
    if (target == n) {
        if (f->erase(f->ctx) != 0) {
            return -1;
        }
        target = 0;
    }

    /* Pad to the programming granularity with 0xFF (programming 0xFF changes nothing). */
    uint8_t buf[BOARD_LOG_SLOT_BYTES];
    memset(buf, 0xFF, sizeof(buf));
    memcpy(buf, data, len);
    size_t padded = (len + 7u) & ~(size_t)7u;

    uint32_t offset = target * BOARD_LOG_SLOT_BYTES;
    /* Everything but the first 8 bytes, then the first 8: that is the commit. */
    if (padded > 8u && f->program(f->ctx, offset + 8u, buf + 8u, padded - 8u) != 0) {
        return -1;
    }
    return f->program(f->ctx, offset, buf, 8u) == 0 ? 0 : -1;
}
