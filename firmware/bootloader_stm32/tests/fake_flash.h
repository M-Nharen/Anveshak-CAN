/* In-RAM flash with NOR semantics for host tests: erase sets a whole unit to 0xFF, program
 * can only clear bits and (like the STM32G4) refuses to program a non-erased location.
 * Layouts mirror the two real cases in docs/architecture.md. */
#ifndef FAKE_FLASH_H
#define FAKE_FLASH_H

#include <stdbool.h>
#include <stdint.h>

#include "anv_bl_flash.h"

#define FAKE_FLASH_MAX_SIZE (448u * 1024u)

typedef struct {
    uint8_t mem[FAKE_FLASH_MAX_SIZE];
    anv_bl_erase_group_t groups[4];
    anv_bl_flash_t flash;
    uint32_t erase_calls;
    uint32_t program_calls;
    bool fail_program; /* simulate a flash error */
} fake_flash_t;

/* G431-like: 46 pages of 2 KB from 0x08009000, 8-byte programming. */
void fake_flash_init_g4(fake_flash_t *ff);
/* F446-like: one 64 KB and three 128 KB sectors from 0x08010000, 4-byte programming. */
void fake_flash_init_f4(fake_flash_t *ff);

#endif /* FAKE_FLASH_H */
