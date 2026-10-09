/* Glue between isotp-c's global shim functions and whoever owns a link. isotp-c is built with
 * ISO_TP_USER_SEND_CAN_ARG, so each link carries a pointer (user_send_can_arg) to one of these;
 * that lets the bootloader and a test's simulated host each run their own links in one
 * process. Time is one shared clock, advanced by the caller. */
#ifndef ANV_ISOTP_PORT_H
#define ANV_ISOTP_PORT_H

#include <stdint.h>

#include "isotp.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct anv_isotp_port_s {
    /* Return ISOTP_RET_OK if queued, ISOTP_RET_NOSPACE to have isotp-c retry later. */
    int (*send)(struct anv_isotp_port_s *self, uint32_t id, const uint8_t *data, uint8_t len);
} anv_isotp_port_t;

void anv_isotp_set_time_ms(uint32_t now_ms);

/* isotp_init_link() plus binding `port` as the link's send target. */
void anv_isotp_link_init(IsoTpLink *link, anv_isotp_port_t *port, uint32_t send_id, uint8_t *send_buf,
                         uint32_t send_size, uint8_t *recv_buf, uint32_t recv_size);

#ifdef __cplusplus
}
#endif

#endif /* ANV_ISOTP_PORT_H */
