/* CAN driver abstraction used by anveshak_can_core.
 *
 * One small API that every backend (STM32 bxCAN, STM32 FDCAN, ESP32-S3 TWAI, and the
 * in-process loopback driver used by tests) implements the same way, so the core library
 * never depends on a specific peripheral.
 */
#ifndef ACC_CAN_H
#define ACC_CAN_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define ACC_CAN_MAX_DLC 8u
#define ACC_CAN_MAX_FILTERS 2u

/* Classic CAN 2.0A, 11-bit identifiers only. */
typedef struct {
    uint16_t id;
    uint8_t dlc;
    uint8_t data[ACC_CAN_MAX_DLC];
} acc_can_frame_t;

/* Two-mask filter recipe: a frame is accepted if (id & mask) == (id_value & mask) for at
 * least one configured filter. n_filters == 0 means "accept everything". */
typedef struct {
    uint16_t id;
    uint16_t mask;
} acc_can_filter_t;

typedef enum {
    ACC_CAN_ERROR_ACTIVE = 0,
    ACC_CAN_ERROR_PASSIVE = 1,
    ACC_CAN_BUS_OFF = 2,
} acc_can_bus_state_t;

typedef struct {
    uint8_t tec;
    uint8_t rec;
    acc_can_bus_state_t state;
    uint32_t bus_off_count;
} acc_can_error_state_t;

typedef enum {
    ACC_CAN_OK = 0,
    ACC_CAN_ERR_INVALID_ARG = -1,
    ACC_CAN_ERR_TX_FULL = -2,
    ACC_CAN_ERR_NOT_READY = -3, /* e.g. bus-off, cannot send */
    ACC_CAN_ERR_BACKEND = -4,
} acc_can_status_t;

/* Software and bus priority order: E-stop first, then feedback, then telemetry, then
 * service/bootloader traffic. Lower enum value = sent first. */
typedef enum {
    ACC_CAN_PRIO_ESTOP = 0,
    ACC_CAN_PRIO_FEEDBACK = 1,
    ACC_CAN_PRIO_TELEMETRY = 2,
    ACC_CAN_PRIO_SERVICE = 3,
    ACC_CAN_PRIO_COUNT = 4,
} acc_can_priority_t;

/* Driver backend vtable. `ctx` is backend-specific state (e.g. a loopback port, or a HAL
 * handle on real hardware) and is passed back into every call. */
typedef struct {
    void *ctx;
    int (*init)(void *ctx, uint32_t bitrate_bps, const acc_can_filter_t *filters, size_t n_filters);
    int (*send)(void *ctx, const acc_can_frame_t *frame, acc_can_priority_t prio);
    bool (*receive)(void *ctx, acc_can_frame_t *frame);
    int (*get_error_state)(void *ctx, acc_can_error_state_t *state);
    int (*start_recovery)(void *ctx);
} acc_can_driver_t;

#ifdef __cplusplus
}
#endif

#endif /* ACC_CAN_H */
