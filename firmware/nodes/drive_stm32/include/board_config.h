/* Picks this build's board variant. Every pin, timer and clock assignment lives in exactly one
 * of the per-board headers below (CLAUDE.md "Build": "Pin and timer assignments in one
 * board_config.h so they're easy to change") -- this file only selects between them, so a new
 * board (or a part swap on an existing one) never means hunting through application code.
 */
#ifndef DRIVE_BOARD_CONFIG_H
#define DRIVE_BOARD_CONFIG_H

#if defined(DRIVE_BOARD_F446)
#include "board_config_f446.h"
#elif defined(DRIVE_BOARD_G431)
#include "board_config_g431.h"
#else
#error "Define DRIVE_BOARD_F446 or DRIVE_BOARD_G431 to select a board variant (see firmware/nodes/drive_stm32/README.md)"
#endif

#endif /* DRIVE_BOARD_CONFIG_H */
