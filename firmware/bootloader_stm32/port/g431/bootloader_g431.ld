/* STM32G431 bootloader: pages 0-15 (32 KB). The link fails if the bootloader outgrows them,
 * which is the 32 KB limit enforced. Pages 16-17 hold parameters, the application starts at
 * page 18 (port/g431/anv_bl_board_g431.c). */
ENTRY(Reset_Handler)

MEMORY
{
  FLASH (rx)  : ORIGIN = 0x08000000, LENGTH = 32K
  RAM   (xrw) : ORIGIN = 0x20000000, LENGTH = 32K
}

INCLUDE anv_stm32_sections.ld
