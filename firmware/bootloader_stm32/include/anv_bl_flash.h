/* Flash backend for the bootloader: the application region only (header slot + image), as a
 * list of erase units. The STM32 ports implement it with the HAL flash driver
 * (port/f446, port/g431); host tests use an in-RAM fake with NOR semantics (tests/).
 * Keeping the bootloader parameters and its own sectors out of this interface means no
 * command can ever reach them, whatever offset a host sends. */
#ifndef ANV_BL_FLASH_H
#define ANV_BL_FLASH_H

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* `count` consecutive erase units of `size` bytes each. */
typedef struct {
    uint16_t count;
    uint32_t size;
} anv_bl_erase_group_t;

typedef struct {
    void *ctx;

    uint32_t base_addr;   /* absolute address of offset 0 (reported in INFO only) */
    uint32_t region_size; /* sum of all erase units */
    uint8_t write_align;  /* programming granularity in bytes (4 on F4, 8 on G4) */

    const anv_bl_erase_group_t *groups;
    uint8_t n_groups;

    /* Erase unit `index` (0-based across all groups). 0 on success. May take seconds (an F4
     * 128 KB sector): callers feed the watchdog around it. */
    int (*erase_unit)(void *ctx, uint32_t index);

    /* Program `len` bytes at `offset`. `offset` and `len` are multiples of write_align and the
     * target is already erased; the caller pads the tail with 0xFF. 0 on success. */
    int (*program)(void *ctx, uint32_t offset, const uint8_t *data, uint32_t len);

    /* Copy `len` bytes from `offset`. */
    void (*read)(void *ctx, uint32_t offset, uint8_t *out, uint32_t len);
} anv_bl_flash_t;

/* Number of erase units, and unit `index`'s offset and size. Returns false if out of range. */
uint32_t anv_bl_flash_unit_count(const anv_bl_flash_t *flash);
bool anv_bl_flash_unit(const anv_bl_flash_t *flash, uint32_t index, uint32_t *offset, uint32_t *size);

/* True if `len` bytes at `offset` all read 0xFF. */
bool anv_bl_flash_is_erased(const anv_bl_flash_t *flash, uint32_t offset, uint32_t len);

/* Erase every unit overlapping [offset, offset + len). `feed` (may be NULL) runs before each
 * unit. 0 on success. */
int anv_bl_flash_erase_range(const anv_bl_flash_t *flash, uint32_t offset, uint32_t len, void (*feed)(void *),
                             void *feed_ctx);

#ifdef __cplusplus
}
#endif

#endif /* ANV_BL_FLASH_H */
