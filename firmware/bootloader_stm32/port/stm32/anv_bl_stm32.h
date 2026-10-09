/* STM32 port pieces shared by the bootloader and the application: the "stay in bootloader"
 * flag (a backup register, so it survives the reset between them), the 96-bit UID and the
 * jump into the application. Family-specific register access lives in anv_bl_stm32.c,
 * selected by ANV_BL_FAMILY_F4 / ANV_BL_FAMILY_G4. */
#ifndef ANV_BL_STM32_H
#define ANV_BL_STM32_H

#include <stdbool.h>
#include <stdint.h>

#if defined(ANV_BL_FAMILY_F4)
#include "stm32f4xx_hal.h"
#elif defined(ANV_BL_FAMILY_G4)
#include "stm32g4xx_hal.h"
#else
#error "Define ANV_BL_FAMILY_F4 or ANV_BL_FAMILY_G4"
#endif

#ifdef __cplusplus
extern "C" {
#endif

/* Application side (ENTER_BOOTLOADER): set the flag. The caller resets afterwards. */
void anv_bl_stay_flag_set(void);

/* Bootloader side: true if the flag was set; clears it so the next reset boots normally. */
bool anv_bl_stay_flag_take(void);

void anv_bl_stm32_read_uid(uint8_t uid[12]);
uint16_t anv_bl_stm32_flash_size_kb(void);

/* Checks the application's vector table (initial SP inside RAM, reset handler inside the
 * image, Thumb bit set) without jumping. */
bool anv_bl_stm32_vector_ok(uint32_t vector_table_addr, uint32_t image_end_addr);

/* Puts clocks and peripherals back to their reset state, points VTOR at the application's
 * vector table and branches to its reset handler. Does not return. */
void anv_bl_stm32_jump(uint32_t vector_table_addr) __attribute__((noreturn));

#ifdef __cplusplus
}
#endif

#endif /* ANV_BL_STM32_H */
