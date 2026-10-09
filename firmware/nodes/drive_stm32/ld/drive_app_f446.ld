/* Drive node application, STM32F446 (512 KB): runs from sector 4 onward, behind the CAN
 * bootloader (sectors 0-1) and the parameter sectors (2-3). docs/architecture.md, "STM32
 * flash layout", F4 column.
 *
 *   0x0801_0000  application header (64 B used, 512 B slot; firmware/bootloader_stm32/
 *                include/anv_bl_proto.h), stamped after linking with length, CRC32, build ID
 *   0x0801_0200  vector table (VTOR, set by SystemInit: VECT_TAB_OFFSET = 0x10200), code
 *
 * The slot is 512 B, not 256 B, because VTOR must be aligned to the vector table's size
 * rounded up to a power of two (113 entries -> 512 B). */
ENTRY(Reset_Handler)

MEMORY
{
  FLASH (rx)  : ORIGIN = 0x08010000, LENGTH = 448K
  RAM   (xrw) : ORIGIN = 0x20000000, LENGTH = 128K
}

SECTIONS
{
  .app_header :
  {
    KEEP(*(.app_header))
    . = 0x200;
  } >FLASH =0xFF
}

INCLUDE anv_stm32_sections.ld

ASSERT(ADDR(.isr_vector) == ORIGIN(FLASH) + 0x200, "vector table must start right after the 512 B header slot")
ASSERT(SIZEOF(.app_header) == 0x200, "header slot must be exactly 512 B")
