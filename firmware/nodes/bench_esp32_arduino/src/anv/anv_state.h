/* Node state machine: BOOT, DISARMED, ARMED, FAULT, ESTOP, BOOTLOADER, with exactly the
 * transitions in the "Node states" table of docs/architecture.md (Node firmware section). */
#ifndef ANV_STATE_H
#define ANV_STATE_H

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Values match the STATE signal in defs/rover.dbc (and NODE_HB byte 0). */
typedef enum {
    ANV_STATE_BOOT = 0,
    ANV_STATE_DISARMED = 1,
    ANV_STATE_ARMED = 2,
    ANV_STATE_FAULT = 3,
    ANV_STATE_ESTOP = 4,
    ANV_STATE_BOOTLOADER = 5,
} anv_state_t;

typedef enum {
    ANV_EVENT_BOOT_COMPLETE,   /* clocks, CAN and ID switch initialised, self-test passed */
    ANV_EVENT_ARM,             /* ARM received from the current master */
    ANV_EVENT_CMD_TIMEOUT,     /* command watchdog expired */
    ANV_EVENT_DISARM,          /* DISARM received, or master/term changed */
    ANV_EVENT_FAULT,           /* over-current, over-temperature, under-voltage, encoder failure */
    ANV_EVENT_FAULT_RESET,     /* fault condition gone and FAULT_RESET received */
    ANV_EVENT_ESTOP,           /* ESTOP frame or the node's own E-stop input */
    ANV_EVENT_ESTOP_RELEASED,  /* ESTOP_RELEASE from the master */
    ANV_EVENT_ENTER_BOOTLOADER,
} anv_event_t;

typedef struct {
    anv_state_t state;
} anv_state_machine_t;

void anv_state_init(anv_state_machine_t *sm);
anv_state_t anv_state_get(const anv_state_machine_t *sm);

/* Applies `event` if it is a valid transition out of the current state. Returns true and
 * updates the state on success; returns false (state unchanged) if the table has no such
 * transition, e.g. ANV_EVENT_ARM while already ARMED. */
bool anv_state_handle(anv_state_machine_t *sm, anv_event_t event);

#ifdef __cplusplus
}
#endif

#endif /* ANV_STATE_H */
