#include "anv_bl_stm32.h"

#include <string.h>

/* RAM bounds from the linker script (anv_stm32_sections.ld). */
extern uint32_t __anv_ram_start;
extern uint32_t __anv_ram_end;

void anv_bl_stm32_read_uid(uint8_t uid[12])
{
    memcpy(uid, (const void *)UID_BASE, 12u);
}

uint16_t anv_bl_stm32_flash_size_kb(void)
{
    return *(const volatile uint16_t *)FLASHSIZE_BASE;
}

bool anv_bl_stm32_vector_ok(uint32_t vector_table_addr, uint32_t image_end_addr)
{
    const volatile uint32_t *vt = (const volatile uint32_t *)vector_table_addr;
    uint32_t sp = vt[0];
    uint32_t pc = vt[1];
    uint32_t ram_start = (uint32_t)&__anv_ram_start;
    uint32_t ram_end = (uint32_t)&__anv_ram_end;
    bool sp_ok = (sp > ram_start) && (sp <= ram_end) && ((sp & 0x3u) == 0u);
    bool pc_ok = ((pc & 1u) != 0u) && (pc > vector_table_addr) && (pc < image_end_addr);
    return sp_ok && pc_ok;
}

void anv_bl_stm32_jump(uint32_t vector_table_addr)
{
    const volatile uint32_t *vt = (const volatile uint32_t *)vector_table_addr;
    uint32_t sp = vt[0];
    uint32_t pc = vt[1];

    /* Leave nothing running that the application doesn't expect: it starts as if from reset. */
    __disable_irq();
    SysTick->CTRL = 0u;
    SysTick->LOAD = 0u;
    SysTick->VAL = 0u;
    (void)HAL_RCC_DeInit();
    (void)HAL_DeInit(); /* force-resets the APB/AHB peripherals, CAN included */
    for (uint32_t i = 0; i < (sizeof(NVIC->ICER) / sizeof(NVIC->ICER[0])); i++) {
        NVIC->ICER[i] = 0xFFFFFFFFu;
        NVIC->ICPR[i] = 0xFFFFFFFFu;
    }
    SCB->ICSR = SCB_ICSR_PENDSTCLR_Msk;

    SCB->VTOR = vector_table_addr;
    __DSB();
    __ISB();
    __enable_irq();

    /* New stack and branch in one asm block: nothing may touch the old stack in between. */
    __asm volatile("msr msp, %0\n"
                   "bx %1\n"
                   :
                   : "r"(sp), "r"(pc)
                   : "memory");
    for (;;) {
    }
}
