#include "anv_isotp_port.h"

#include <stdarg.h>

static uint32_t s_now_us;

void anv_isotp_set_time_ms(uint32_t now_ms)
{
    /* Wraps every ~71 minutes; isotp-c compares deadlines wrap-safely. */
    s_now_us = now_ms * 1000u;
}

void anv_isotp_link_init(IsoTpLink *link, anv_isotp_port_t *port, uint32_t send_id, uint8_t *send_buf,
                         uint32_t send_size, uint8_t *recv_buf, uint32_t recv_size)
{
    isotp_init_link(link, send_id, send_buf, send_size, recv_buf, recv_size);
    link->user_send_can_arg = port;
}

/* ---- isotp-c shim (isotp_user.h) -------------------------------------------------------- */

void isotp_user_debug(const char *message, ...)
{
    (void)message; /* no console in the bootloader */
}

int isotp_user_send_can(const uint32_t arbitration_id, const uint8_t *data, const uint8_t size, void *arg)
{
    anv_isotp_port_t *port = (anv_isotp_port_t *)arg;
    if (port == NULL || port->send == NULL) {
        return ISOTP_RET_ERROR;
    }
    return port->send(port, arbitration_id, data, size);
}

uint32_t isotp_user_get_us(void)
{
    return s_now_us;
}
