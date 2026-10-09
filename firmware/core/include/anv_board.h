/* Board hooks: the small set of functions every node application implements so the core
 * library stays portable across STM32 and ESP32-S3 boards (and runs on Linux in tests). */
#ifndef ANV_BOARD_H
#define ANV_BOARD_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Why the MCU last reset. Reported in DRIVE_TELEM_1.last_reset_cause and used to raise
 * BROWNOUT_RESET / WATCHDOG_RESET / HARD_FAULT / STACK_OVERFLOW after boot. */
typedef enum {
    ANV_RESET_UNKNOWN = 0,
    ANV_RESET_POWER_ON = 1,
    ANV_RESET_BROWNOUT = 2,
    ANV_RESET_WATCHDOG = 3,
    ANV_RESET_SOFTWARE = 4,
    ANV_RESET_HARD_FAULT = 5,
    ANV_RESET_PIN = 6,
    ANV_RESET_PANIC = 7,          /* ESP32-S3 panic handler */
    ANV_RESET_STACK_OVERFLOW = 8, /* ESP32-S3 / RTOS */
} anv_reset_cause_t;

/* What the crash handler saved before resetting. STM32: Cortex-M fault registers copied by the
 * HardFault handler into no-init RAM. ESP32-S3: pc is the panic program counter, the other
 * fields are 0 (the full core dump stays in its flash partition). */
typedef struct {
    uint32_t pc;
    uint32_t lr;
    uint32_t xpsr;
    uint32_t cfsr;
    uint32_t hfsr;
    uint32_t mmfar;
    uint32_t bfar;
    uint32_t extra; /* breadcrumb of the last loop/task that ran, if the firmware sets one */
} anv_crash_record_t;

typedef struct {
    void *ctx;

    /* Read the node ID switch/jumpers. Called once at boot by the application; the core
     * does not call this itself. */
    uint8_t (*read_node_id)(void *ctx);

    /* Force all outputs off immediately (coast or brake, per a node parameter). The core
     * calls this on every transition away from ARMED. */
    void (*set_outputs_off)(void *ctx);

    /* Feed the hardware watchdog (STM32 IWDG / ESP32-S3 task watchdog). Call from the main
     * loop only, never from an interrupt. */
    void (*feed_hw_watchdog)(void *ctx);

    /* Monotonic milliseconds since boot. */
    uint32_t (*now_ms)(void *ctx);

    /* Set the "stay in bootloader" flag (e.g. a backup register) read by the bootloader on
     * the next reset. Optional; may be NULL. */
    void (*enter_bootloader)(void *ctx);

    /* Request an MCU reset (NMT RESET). Optional; may be NULL. */
    void (*request_reset)(void *ctx);

    /* Persistent parameter storage (flash on boards, a file or RAM in tests). Return 0 on
     * success, negative on failure. load() returns -1 if nothing has been saved yet. */
    int (*storage_load)(void *ctx, uint8_t *data, size_t len);
    int (*storage_save)(void *ctx, const uint8_t *data, size_t len);

    /* --- Diagnostics (anv_diag.h). All optional (NULL = feature off), appended here so board
     * code written before they existed keeps working. --- */

    /* Flash black-box log: one blob, same load/save contract as storage_*. STM32: a reserved
     * flash page; ESP32-S3: an NVS blob. Called rarely (see anv_diag.h on flash wear). */
    int (*log_load)(void *ctx, uint8_t *data, size_t len);
    int (*log_save)(void *ctx, const uint8_t *data, size_t len);

    /* If the last reset was a crash, copy the saved record out, clear it and return true. */
    bool (*take_crash_record)(void *ctx, anv_crash_record_t *out);

    /* anv_reset_cause_t of the last reset. */
    uint8_t (*reset_cause)(void *ctx);

    /* Power-on self-test: check what can be checked without moving anything and return a
     * bitmask of failed tests, bit n = test number n + 1 (1 supply voltage, 2 driver fault pin,
     * 3 encoder lines, 4 sensors, 5 parameter CRC, 6 node ID switch). 0 = all passed. */
    uint32_t (*self_test)(void *ctx);
} anv_board_hooks_t;

#ifdef __cplusplus
}
#endif

#endif /* ANV_BOARD_H */
