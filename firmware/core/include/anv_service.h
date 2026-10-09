/* Generic handling of the SERVICE opcodes that touch the parameter store (GET, SET, SAVE,
 * RESTORE_DEFAULTS, INFO). The request/response here are the SERVICE payload already
 * unpacked by the application from its generated per-node message (the layout is identical
 * for every node and requester per docs/architecture.md, "SERVICE: requests and responses
 * share one layout"); this module only reasons about opcode/index/value, not CAN framing. */
#ifndef ANV_SERVICE_H
#define ANV_SERVICE_H

#include <stdbool.h>
#include <stdint.h>

#include "anv_params.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    ANV_SERVICE_OP_GET = 1,
    ANV_SERVICE_OP_SET = 2,
    ANV_SERVICE_OP_SAVE = 3,
    ANV_SERVICE_OP_RESTORE_DEFAULTS = 4,
    ANV_SERVICE_OP_INFO = 5,
    ANV_SERVICE_OP_LOG_READ = 6,
    ANV_SERVICE_OP_LOG_CLEAR = 7,
} anv_service_opcode_t;

typedef struct {
    uint8_t opcode;
    uint8_t transaction_id;
    uint16_t param_index;
    uint32_t value_raw;
} anv_service_request_t;

typedef struct {
    uint8_t opcode;
    uint8_t status;
    uint8_t transaction_id;
    uint16_t param_index;
    uint32_t value_raw;
} anv_service_response_t;

/* Fixed INFO pages (docs/architecture.md: "INFO uses the index field to page through fixed
 * information"). Paging the full git hash across two responses is left to a later prompt;
 * index 0 here returns a single 32-bit build id, matching NODE_HB's fw_build_id. */
typedef struct {
    uint32_t git_build_id;
    uint32_t firmware_version;
    uint32_t mcu_unique_id;
    uint32_t build_date;
} anv_node_info_t;

/* Handles GET/SET/SAVE/RESTORE_DEFAULTS/INFO against `store`, filling `resp`. Returns false
 * (resp untouched) for LOG_READ/LOG_CLEAR or an unknown opcode, which the core does not
 * implement; the caller then simply sends no response. */
bool anv_service_handle_request(anv_param_store_t *store, bool armed, const anv_node_info_t *info,
                                 const anv_service_request_t *req, anv_service_response_t *resp);

#ifdef __cplusplus
}
#endif

#endif /* ANV_SERVICE_H */
