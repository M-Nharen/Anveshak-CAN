/* STM32F446 bootloader: sectors 0-1 (32 KB). The link fails if the bootloader outgrows them,
 * which is the 32 KB limit enforced. Sectors 2-3 hold parameters, the application starts at
 * sector 4 (port/f446/anv_bl_board_f446.c). */
ENTRY(Reset_Handler)

MEMORY
{
  FLASH (rx)  : ORIGIN = 0x08000000, LENGTH = 32K
  RAM   (xrw) : ORIGIN = 0x20000000, LENGTH = 128K
}

INCLUDE anv_stm32_sections.ld
