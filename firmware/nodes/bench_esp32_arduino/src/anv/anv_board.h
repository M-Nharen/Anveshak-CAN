/* Board hooks: the small set of functions every node application implements so the core
 * library stays portable across STM32 and ESP32-S3 boards (and runs on Linux in tests). */
#ifndef ANV_BOARD_H
#define ANV_BOARD_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

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
} anv_board_hooks_t;

#ifdef __cplusplus
}
#endif

#endif /* ANV_BOARD_H */
