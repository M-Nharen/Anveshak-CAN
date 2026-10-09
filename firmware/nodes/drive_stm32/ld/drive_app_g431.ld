/* Drive node application, STM32G431 (128 KB): runs from page 18 onward, behind the CAN
 * bootloader (pages 0-15) and the parameter pages (16-17). docs/architecture.md, "STM32
 * flash layout", 2 KB-page column.
 *
 *   0x0800_9000  application header (64 B used, 512 B slot; firmware/bootloader_stm32/
 *                include/anv_bl_proto.h), stamped after linking with length, CRC32, build ID
 *   0x0800_9200  vector table (VTOR, set by SystemInit: VECT_TAB_OFFSET = 0x9200), code
 *
 * The slot is 512 B, not 256 B, because VTOR must be aligned to the vector table's size
 * rounded up to a power of two (118 entries -> 512 B). */
ENTRY(Reset_Handler)

MEMORY
{
  FLASH (rx)  : ORIGIN = 0x08009000, LENGTH = 92K
  RAM   (xrw) : ORIGIN = 0x20000000, LENGTH = 32K
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
