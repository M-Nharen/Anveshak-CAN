/* The "stay in bootloader" flag: linked into both the bootloader and the application. */
#include "anv_bl_stm32.h"

#include "anv_bl_proto.h"

/* Backup register 0 survives a system reset (and keeps its value as long as VDD or VBAT is
 * present). Writing it needs the PWR clock and the backup-domain write enable (DBP); on G4
 * the backup registers live in TAMP, behind the RTC APB clock. */
static volatile uint32_t *backup_reg0(void)
{
    __HAL_RCC_PWR_CLK_ENABLE();
#if defined(ANV_BL_FAMILY_G4)
    __HAL_RCC_RTCAPB_CLK_ENABLE();
    HAL_PWR_EnableBkUpAccess();
    return &TAMP->BKP0R;
#else
    HAL_PWR_EnableBkUpAccess();
    return &RTC->BKP0R;
#endif
}

void anv_bl_stay_flag_set(void)
{
    *backup_reg0() = ANV_BL_STAY_FLAG_MAGIC;
}

bool anv_bl_stay_flag_take(void)
{
    volatile uint32_t *reg = backup_reg0();
    bool set = (*reg == ANV_BL_STAY_FLAG_MAGIC);
    *reg = 0u;
    return set;
}
