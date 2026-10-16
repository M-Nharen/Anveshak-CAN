#include "anv_service.h"

static anv_param_value_t raw_to_value(uint32_t raw)
{
    anv_param_value_t v;
    v.u32 = raw; /* union: same bit pattern, interpreted per descriptor type elsewhere */
    return v;
}

static uint32_t value_to_raw(anv_param_value_t v)
{
    return v.u32;
}

bool anv_service_handle_request(anv_param_store_t *store, bool armed, const anv_node_info_t *info,
                                 const anv_service_request_t *req, anv_service_response_t *resp)
{
    resp->opcode = req->opcode;
    resp->transaction_id = req->transaction_id;
    resp->param_index = req->param_index;
    resp->value_raw = 0;

    switch ((anv_service_opcode_t)req->opcode) {
    case ANV_SERVICE_OP_GET: {
        anv_param_value_t value;
        anv_param_status_t status = anv_param_get(store, req->param_index, &value);
        resp->status = (uint8_t)status;
        if (status == ANV_PARAM_STATUS_OK) {
            resp->value_raw = value_to_raw(value);
        }
        return true;
    }
    case ANV_SERVICE_OP_SET: {
        anv_param_status_t status = anv_param_set(store, req->param_index, raw_to_value(req->value_raw), armed);
        resp->status = (uint8_t)status;
        if (status == ANV_PARAM_STATUS_OK) {
            resp->value_raw = req->value_raw;
        }
        return true;
    }
    case ANV_SERVICE_OP_SAVE: {
        anv_param_status_t status = anv_param_save(store);
        resp->status = (uint8_t)status;
        return true;
    }
    case ANV_SERVICE_OP_RESTORE_DEFAULTS: {
        anv_param_status_t status = anv_param_restore_defaults(store, armed);
        resp->status = (uint8_t)status;
        return true;
    }
    case ANV_SERVICE_OP_INFO: {
        resp->status = (uint8_t)ANV_PARAM_STATUS_OK;
        if (info == NULL) {
            resp->status = (uint8_t)ANV_PARAM_STATUS_BAD_INDEX;
            return true;
        }
        switch (req->param_index) {
        case 0:
            resp->value_raw = info->git_build_id;
            break;
        case 2:
            resp->value_raw = info->firmware_version;
            break;
        case 3:
            resp->value_raw = info->mcu_unique_id;
            break;
        case 4:
            resp->value_raw = info->build_date;
            break;
        default:
            resp->status = (uint8_t)ANV_PARAM_STATUS_BAD_INDEX;
            break;
        }
        return true;
    }
    case ANV_SERVICE_OP_LOG_READ:
    case ANV_SERVICE_OP_LOG_CLEAR:
    default:
        return false;
    }
}
