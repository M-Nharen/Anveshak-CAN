/* What the drive application needs to live behind the CAN bootloader: its image header
 * (placed first in flash by ld/drive_app_*.ld, stamped after linking), the board hooks for
 * ENTER_BOOTLOADER and RESET, and the SysTick handler a linked image needs. Shared by the F446
 * and G431 variants. */
#ifndef DRIVE_APP_IMAGE_H
#define DRIVE_APP_IMAGE_H

#include <stdint.h>

/* The build ID stamped into this image's header (NODE_HB reports its low 16 bits), read
 * from flash at run time so it is the stamped value, not the link-time placeholder. */
uint32_t app_image_build_id(void);

/* anv_board_hooks_t.enter_bootloader: sets the bootloader's "stay" flag. */
void app_image_enter_bootloader(void *ctx);

/* anv_board_hooks_t.request_reset. */
void app_image_request_reset(void *ctx);

#endif /* DRIVE_APP_IMAGE_H */
