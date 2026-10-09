#include "anv_bl_flash.h"
#include <stddef.h>

uint32_t anv_bl_flash_unit_count(const anv_bl_flash_t *flash)
{
    uint32_t n = 0u;
    for (uint8_t g = 0; g < flash->n_groups; g++) {
        n += flash->groups[g].count;
    }
    return n;
}

bool anv_bl_flash_unit(const anv_bl_flash_t *flash, uint32_t index, uint32_t *offset, uint32_t *size)
{
    uint32_t off = 0u;
    for (uint8_t g = 0; g < flash->n_groups; g++) {
        const anv_bl_erase_group_t *grp = &flash->groups[g];
        if (index < grp->count) {
            *offset = off + index * grp->size;
            *size = grp->size;
            return true;
        }
        index -= grp->count;
        off += (uint32_t)grp->count * grp->size;
    }
    return false;
}

bool anv_bl_flash_is_erased(const anv_bl_flash_t *flash, uint32_t offset, uint32_t len)
{
    uint8_t chunk[32];
    while (len > 0u) {
        uint32_t n = (len < sizeof(chunk)) ? len : (uint32_t)sizeof(chunk);
        flash->read(flash->ctx, offset, chunk, n);
        for (uint32_t i = 0; i < n; i++) {
            if (chunk[i] != 0xFFu) {
                return false;
            }
        }
        offset += n;
        len -= n;
    }
    return true;
}

int anv_bl_flash_erase_range(const anv_bl_flash_t *flash, uint32_t offset, uint32_t len, void (*feed)(void *),
                             void *feed_ctx)
{
    uint32_t end = offset + len;
    uint32_t count = anv_bl_flash_unit_count(flash);
    for (uint32_t i = 0; i < count; i++) {
        uint32_t u_off;
        uint32_t u_size;
        (void)anv_bl_flash_unit(flash, i, &u_off, &u_size);
        if (u_off >= end || u_off + u_size <= offset) {
            continue;
        }
        if (feed != NULL) {
            feed(feed_ctx);
        }
        if (flash->erase_unit(flash->ctx, i) != 0) {
            return -1;
        }
    }
    if (feed != NULL) {
        feed(feed_ctx);
    }
    return 0;
}
