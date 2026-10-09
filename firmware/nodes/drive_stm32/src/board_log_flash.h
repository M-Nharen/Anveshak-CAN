/* Black-box log storage in a reserved flash region (anv_board_hooks_t log_load / log_save).
 *
 * Flash can only be erased in big blocks and only a few thousand times, so the region is used
 * as a ring of fixed-size slots: every save appends the whole log blob to the next blank slot
 * and the region is erased only when it is full. With a 128 KB sector that is one erase per 256
 * saves. A save is "committed" by programming the first word last, so a power cut half way
 * through leaves a slot that looks blank to load() and is skipped, never a half-written log.
 *
 * Family-specific parts (erase, program, unlock) are two callbacks; everything else is plain C
 * that the host tests run against a RAM array. */
#ifndef BOARD_LOG_FLASH_H
#define BOARD_LOG_FLASH_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define BOARD_LOG_SLOT_BYTES 512u

typedef struct {
    const uint8_t *mem; /* memory-mapped view of the region (flash reads like RAM) */
    uint32_t size;      /* bytes, a multiple of BOARD_LOG_SLOT_BYTES */
    void *ctx;
    int (*erase)(void *ctx);                                                     /* whole region -> 0xFF */
    int (*program)(void *ctx, uint32_t offset, const uint8_t *data, size_t len); /* offset/len multiples of 8 */
} board_log_flash_t;

/* Both match anv_board_hooks_t's log_load / log_save contract (0 = ok). The `ctx` argument is
 * a board_log_flash_t *, so board code can wrap them in two one-line hooks. */
int board_log_flash_load(void *ctx, uint8_t *data, size_t len);
int board_log_flash_save(void *ctx, const uint8_t *data, size_t len);

#ifdef __cplusplus
}
#endif

#endif /* BOARD_LOG_FLASH_H */
