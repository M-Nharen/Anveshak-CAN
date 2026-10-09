#include "board_crash.h"

#include <string.h>

#if defined(DRIVE_BOARD_F446)
#include "stm32f4xx_hal.h"
#elif defined(DRIVE_BOARD_G431)
#include "stm32g4xx_hal.h"
#else
#error "Define DRIVE_BOARD_F446 or DRIVE_BOARD_G431"
#endif

#define CRASH_MAGIC 0xC4A5D00Du

/* The check word guards against random RAM at power-on that happens to hold the magic. */
typedef struct {
    uint32_t magic;
    anv_crash_record_t rec;
    uint32_t check;
} crash_slot_t;

static crash_slot_t s_crash __attribute__((section(".noinit")));
static uint8_t s_reset_cause;
static bool s_crash_pending;

static uint32_t slot_check(const crash_slot_t *s)
{
    const uint32_t *w = (const uint32_t *)&s->rec;
    uint32_t x = s->magic ^ 0x5A5A5A5Au;
    for (size_t i = 0; i < sizeof(s->rec) / sizeof(uint32_t); i++) {
        x = (x << 1) ^ (x >> 31) ^ w[i];
    }
    return x;
}

void board_crash_capture(const uint32_t *stacked_frame, uint32_t cfsr, uint32_t hfsr, uint32_t mmfar, uint32_t bfar)
{
    /* Exception frame: r0 r1 r2 r3 r12 lr pc xpsr. */
    s_crash.rec.lr = stacked_frame[5];
    s_crash.rec.pc = stacked_frame[6];
    s_crash.rec.xpsr = stacked_frame[7];
    s_crash.rec.cfsr = cfsr;
    s_crash.rec.hfsr = hfsr;
    s_crash.rec.mmfar = mmfar;
    s_crash.rec.bfar = bfar;
    s_crash.rec.extra = 0u;
    s_crash.magic = CRASH_MAGIC;
    s_crash.check = slot_check(&s_crash); /* last: a half-written record is never valid */
}

void board_hardfault_c(const uint32_t *stacked_frame) __attribute__((used));

/* Picks the stack the fault happened on (MSP or PSP, bit 2 of LR) and hands it to C. */
__attribute__((naked)) void HardFault_Handler(void)
{
    __asm volatile(".syntax unified\n"
                   "tst lr, #4\n"
                   "ite eq\n"
                   "mrseq r0, msp\n"
                   "mrsne r0, psp\n"
                   "b board_hardfault_c\n");
}

void board_hardfault_c(const uint32_t *stacked_frame)
{
    board_crash_capture(stacked_frame, SCB->CFSR, SCB->HFSR, SCB->MMFAR, SCB->BFAR);
    NVIC_SystemReset();
    for (;;) {
    }
}

void board_crash_init(void)
{
    s_crash_pending = (s_crash.magic == CRASH_MAGIC) && (s_crash.check == slot_check(&s_crash));

    uint32_t csr = RCC->CSR;
    RCC->CSR |= RCC_CSR_RMVF; /* clear the flags so the next reset starts clean */

    if (s_crash_pending) {
        s_reset_cause = (uint8_t)ANV_RESET_HARD_FAULT; /* the fault handler's NVIC_SystemReset also sets SFTRSTF */
    } else if ((csr & (RCC_CSR_IWDGRSTF | RCC_CSR_WWDGRSTF)) != 0u) {
        s_reset_cause = (uint8_t)ANV_RESET_WATCHDOG;
#if defined(RCC_CSR_PORRSTF)
    } else if ((csr & RCC_CSR_PORRSTF) != 0u) {
        s_reset_cause = (uint8_t)ANV_RESET_POWER_ON;
    } else if ((csr & RCC_CSR_BORRSTF) != 0u) {
        s_reset_cause = (uint8_t)ANV_RESET_BROWNOUT; /* BOR without POR: the supply dipped */
#else
    } else if ((csr & RCC_CSR_BORRSTF) != 0u) {
        /* G4 sets BORRSTF for power-on and brown-out alike and has no separate POR flag, so the
         * two can't be told apart here; report the common case. */
        s_reset_cause = (uint8_t)ANV_RESET_POWER_ON;
#endif
    } else if ((csr & RCC_CSR_SFTRSTF) != 0u) {
        s_reset_cause = (uint8_t)ANV_RESET_SOFTWARE;
    } else if ((csr & RCC_CSR_PINRSTF) != 0u) {
        s_reset_cause = (uint8_t)ANV_RESET_PIN;
    } else {
        s_reset_cause = (uint8_t)ANV_RESET_UNKNOWN;
    }
}

bool board_crash_take(void *ctx, anv_crash_record_t *out)
{
    (void)ctx;
    if (!s_crash_pending) {
        return false;
    }
    *out = s_crash.rec;
    s_crash_pending = false;
    memset(&s_crash, 0, sizeof(s_crash)); /* one report per crash */
    return true;
}

uint8_t board_crash_reset_cause(void *ctx)
{
    (void)ctx;
    return s_reset_cause;
}
