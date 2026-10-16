#include "anv_state.h"

void anv_state_init(anv_state_machine_t *sm)
{
    sm->state = ANV_STATE_BOOT;
}

anv_state_t anv_state_get(const anv_state_machine_t *sm)
{
    return sm->state;
}

bool anv_state_handle(anv_state_machine_t *sm, anv_event_t event)
{
    /* ESTOP is a safety override: accepted from every state, including a no-op from ESTOP
     * itself. Everything else follows the Node states table exactly. */
    if (event == ANV_EVENT_ESTOP) {
        sm->state = ANV_STATE_ESTOP;
        return true;
    }

    switch (sm->state) {
    case ANV_STATE_BOOT:
        if (event == ANV_EVENT_BOOT_COMPLETE) {
            sm->state = ANV_STATE_DISARMED;
            return true;
        }
        break;

    case ANV_STATE_DISARMED:
        if (event == ANV_EVENT_ARM) {
            sm->state = ANV_STATE_ARMED;
            return true;
        }
        if (event == ANV_EVENT_FAULT) {
            sm->state = ANV_STATE_FAULT;
            return true;
        }
        if (event == ANV_EVENT_ENTER_BOOTLOADER) {
            sm->state = ANV_STATE_BOOTLOADER;
            return true;
        }
        break;

    case ANV_STATE_ARMED:
        if (event == ANV_EVENT_CMD_TIMEOUT || event == ANV_EVENT_DISARM) {
            sm->state = ANV_STATE_DISARMED;
            return true;
        }
        if (event == ANV_EVENT_FAULT) {
            sm->state = ANV_STATE_FAULT;
            return true;
        }
        break;

    case ANV_STATE_FAULT:
        if (event == ANV_EVENT_FAULT_RESET) {
            sm->state = ANV_STATE_DISARMED;
            return true;
        }
        break;

    case ANV_STATE_ESTOP:
        if (event == ANV_EVENT_ESTOP_RELEASED) {
            sm->state = ANV_STATE_DISARMED;
            return true;
        }
        break;

    case ANV_STATE_BOOTLOADER:
        /* Left only by a reboot into committed new firmware; no event here does that. */
        break;
    }

    return false;
}
