/* Crash record and reset cause for the STM32 drive boards (Cortex-M4: F446 and G431).
 *
 * The HardFault handler copies the stacked registers and the fault status registers into a
 * variable in .noinit RAM (not cleared by startup code, and kept through a soft reset), then
 * resets the chip. After the reset the application reads it back through the board hooks and
 * reports a HARD_FAULT event (and the INFO pages) with the PC and fault status of the crash.
 *
 * Needs the linker script to keep `.noinit` out of .bss/.data initialisation -- the usual
 * ST linker scripts already do. A real board project supplies that script (see README.md). */
#ifndef BOARD_CRASH_H
#define BOARD_CRASH_H

#include <stdbool.h>
#include <stdint.h>

#include "anv_board.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Reads and clears the reset flags. Call first thing in board_hw_init(), before anything that
 * could reset the chip again. */
void board_crash_init(void);

/* anv_board_hooks_t take_crash_record / reset_cause (the ctx argument is unused). */
bool board_crash_take(void *ctx, anv_crash_record_t *out);
uint8_t board_crash_reset_cause(void *ctx);

/* Called from the HardFault_Handler assembly stub with the stack pointer in use at the fault.
 * Exposed so it can be unit-tested without a Cortex-M. */
void board_crash_capture(const uint32_t *stacked_frame, uint32_t cfsr, uint32_t hfsr, uint32_t mmfar, uint32_t bfar);

#ifdef __cplusplus
}
#endif

#endif /* BOARD_CRASH_H */
