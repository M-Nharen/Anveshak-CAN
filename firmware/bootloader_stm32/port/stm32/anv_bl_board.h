/* What each STM32 family file (port/f446, port/g431) provides to the shared bootloader main
 * loop (anv_bl_main.c). Pins come from the node's board_config.h -- the same file the
 * application uses -- so the bootloader can never disagree with it about CAN or ID pins. */
#ifndef ANV_BL_BOARD_H
#define ANV_BL_BOARD_H

#include <stdint.h>

#include "acc_can.h"
#include "anv_bl_flash.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Clocks, then GPIO for the CAN pins, the transceiver's STB (driven low) and the node ID
 * switch. Nothing else: motor-driver pins keep their reset state (inputs), and the hardware
 * pull-downs keep the drivers disabled. */
void anv_bl_board_init(void);

/* Starts CAN at 500 kbps with `filters` (anv_bl_can_filters()) and returns the driver. */
const acc_can_driver_t *anv_bl_board_can_start(const acc_can_filter_t filters[2]);

/* The application region of this part's flash (layout depends on the flash size). */
const anv_bl_flash_t *anv_bl_board_flash(uint16_t flash_size_kb);

#ifdef __cplusplus
}
#endif

#endif /* ANV_BL_BOARD_H */
