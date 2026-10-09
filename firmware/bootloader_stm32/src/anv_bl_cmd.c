#include "anv_bl_cmd.h"

#include <string.h>

#include "anv_bl_image.h"
#include "errors.h"

static uint32_t rd32(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static void wr16(uint8_t *p, uint16_t v)
{
    p[0] = (uint8_t)v;
    p[1] = (uint8_t)(v >> 8);
}

static void wr32(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)v;
    p[1] = (uint8_t)(v >> 8);
    p[2] = (uint8_t)(v >> 16);
    p[3] = (uint8_t)(v >> 24);
}

/* The errors.yaml code a host should show for each failure status. */
static uint16_t status_error_code(uint8_t status)
{
    switch (status) {
    case ANV_BL_ST_OK:
        return 0u;
    case ANV_BL_ST_MISMATCH:
    case ANV_BL_ST_BAD_IMAGE:
        return ROVER_ERR_APP_CRC_FAIL;
    case ANV_BL_ST_WRONG_NODE_TYPE:
        return ROVER_ERR_WRONG_NODE_TYPE;
    case ANV_BL_ST_NOT_ERASED:
    case ANV_BL_ST_FLASH_ERROR:
        return ROVER_ERR_FLASH_WRITE_FAIL;
    default:
        return ROVER_ERR_BAD_FRAME;
    }
}

static size_t respond(uint8_t *resp, uint8_t cmd, uint8_t status, uint16_t error_code)
{
    resp[0] = cmd;
    resp[1] = status;
    wr16(&resp[2], (status == ANV_BL_ST_OK) ? 0u : error_code);
    return ANV_BL_RESP_PREFIX_LEN;
}

static size_t respond_status(uint8_t *resp, uint8_t cmd, uint8_t status)
{
    return respond(resp, cmd, status, status_error_code(status));
}

static void touch(anv_bl_cmd_t *c, uint32_t end)
{
    if (end > c->touched_end) {
        c->touched_end = end;
    }
}

static void reset_session(anv_bl_cmd_t *c)
{
    c->have_header = false;
    c->verified = false;
    c->touched_end = 0u;
    memset(c->header_raw, 0xFF, sizeof(c->header_raw));
}

void anv_bl_cmd_init(anv_bl_cmd_t *c, const anv_bl_flash_t *flash, uint8_t node_id, uint8_t node_type,
                     const uint8_t uid[12], uint16_t flash_size_kb)
{
    memset(c, 0, sizeof(*c));
    c->flash = flash;
    c->node_id = node_id;
    c->node_type = node_type;
    memcpy(c->uid, uid, sizeof(c->uid));
    c->uid32 = anv_bl_uid32(uid);
    c->flash_size_kb = flash_size_kb;
    reset_session(c);
}

uint32_t anv_bl_uid32(const uint8_t uid[12])
{
    return anv_bl_crc32(0u, uid, 12u);
}

static size_t handle_info(anv_bl_cmd_t *c, uint8_t *resp)
{
    const anv_bl_flash_t *f = c->flash;
    size_t n = respond(resp, ANV_BL_CMD_INFO, ANV_BL_ST_OK, 0u);
    uint8_t *p = &resp[n];

    wr16(&p[0], ANV_BL_VERSION);
    p[2] = c->node_id;
    p[3] = c->node_type;
    memcpy(&p[4], c->uid, 12u);
    wr32(&p[16], c->uid32);
    wr16(&p[20], c->flash_size_kb);
    wr32(&p[22], f->base_addr);
    wr32(&p[26], f->region_size);
    wr16(&p[30], ANV_BL_HEADER_SLOT);
    p[32] = f->write_align;
    wr16(&p[33], ANV_BL_MAX_WRITE);
    wr16(&p[35], anv_bl_app_check(f, c->node_type, NULL));

    uint8_t groups = (f->n_groups > ANV_BL_MAX_LAYOUT_GROUPS) ? ANV_BL_MAX_LAYOUT_GROUPS : f->n_groups;
    p[37] = groups;
    size_t off = ANV_BL_INFO_FIXED_LEN;
    for (uint8_t g = 0; g < groups; g++) {
        wr16(&p[off], f->groups[g].count);
        wr32(&p[off + 2u], f->groups[g].size);
        off += 6u;
    }
    f->read(f->ctx, 0u, &p[off], ANV_BL_HEADER_LEN);
    off += ANV_BL_HEADER_LEN;
    return n + off;
}

static size_t handle_erase(anv_bl_cmd_t *c, const uint8_t *req, size_t len, uint8_t *resp)
{
    if (len != 9u) {
        return respond_status(resp, ANV_BL_CMD_ERASE, ANV_BL_ST_BAD_LENGTH);
    }
    uint32_t offset = rd32(&req[1]);
    uint32_t length = rd32(&req[5]);
    uint32_t region = c->flash->region_size;
    if (length == 0u || offset >= region || length > region - offset) {
        return respond_status(resp, ANV_BL_CMD_ERASE, ANV_BL_ST_BAD_RANGE);
    }
    c->verified = false;
    touch(c, offset + length);
    if (anv_bl_flash_erase_range(c->flash, offset, length, c->feed_watchdog, c->feed_ctx) != 0) {
        return respond_status(resp, ANV_BL_CMD_ERASE, ANV_BL_ST_FLASH_ERROR);
    }
    return respond_status(resp, ANV_BL_CMD_ERASE, ANV_BL_ST_OK);
}

/* Programs `n` bytes at an aligned `offset` (already checked erased), padding the last partial
 * unit with 0xFF, then reads everything back. */
static uint8_t program_and_verify(anv_bl_cmd_t *c, uint32_t offset, const uint8_t *data, uint32_t n)
{
    const anv_bl_flash_t *f = c->flash;
    uint32_t align = f->write_align;
    uint32_t whole = n - (n % align);

    if (whole > 0u && f->program(f->ctx, offset, data, whole) != 0) {
        return ANV_BL_ST_FLASH_ERROR;
    }
    if (whole < n) {
        uint8_t tail[16];
        memset(tail, 0xFF, sizeof(tail));
        memcpy(tail, &data[whole], n - whole);
        if (f->program(f->ctx, offset + whole, tail, align) != 0) {
            return ANV_BL_ST_FLASH_ERROR;
        }
    }

    uint8_t chunk[32];
    for (uint32_t done = 0; done < n;) {
        uint32_t k = ((n - done) < sizeof(chunk)) ? (n - done) : (uint32_t)sizeof(chunk);
        f->read(f->ctx, offset + done, chunk, k);
        if (memcmp(chunk, &data[done], k) != 0) {
            return ANV_BL_ST_FLASH_ERROR;
        }
        done += k;
    }
    return ANV_BL_ST_OK;
}

static size_t handle_write(anv_bl_cmd_t *c, const uint8_t *req, size_t len, uint8_t *resp)
{
    const anv_bl_flash_t *f = c->flash;
    if (len <= ANV_BL_WRITE_OVERHEAD || len > ANV_BL_MAX_REQUEST) {
        return respond_status(resp, ANV_BL_CMD_WRITE, ANV_BL_ST_BAD_LENGTH);
    }
    uint32_t offset = rd32(&req[1]);
    const uint8_t *data = &req[ANV_BL_WRITE_OVERHEAD];
    uint32_t n = (uint32_t)(len - ANV_BL_WRITE_OVERHEAD);

    if (offset >= f->region_size || n > f->region_size - offset || (offset % f->write_align) != 0u ||
        f->write_align > 16u) {
        return respond_status(resp, ANV_BL_CMD_WRITE, ANV_BL_ST_BAD_RANGE);
    }

    if (offset < ANV_BL_HEADER_SLOT) {
        /* The header slot is never programmed by WRITE: the header is checked and kept in RAM
         * until COMMIT. It must arrive whole, at the very start of the image. */
        if (offset != 0u || n < ANV_BL_HEADER_LEN) {
            return respond_status(resp, ANV_BL_CMD_WRITE, ANV_BL_ST_BAD_RANGE);
        }
        anv_bl_app_header_t hdr;
        anv_bl_header_decode(data, &hdr);
        uint16_t err = anv_bl_header_check(&hdr, c->node_type, f->region_size);
        if (err == ROVER_ERR_WRONG_NODE_TYPE) {
            return respond_status(resp, ANV_BL_CMD_WRITE, ANV_BL_ST_WRONG_NODE_TYPE);
        }
        if (err != 0u) {
            return respond_status(resp, ANV_BL_CMD_WRITE, ANV_BL_ST_BAD_IMAGE);
        }
        memcpy(c->header_raw, data, ANV_BL_HEADER_LEN);
        c->have_header = true;
        c->verified = false;

        uint32_t skip = (n < ANV_BL_HEADER_SLOT) ? n : ANV_BL_HEADER_SLOT;
        offset += skip;
        data += skip;
        n -= skip;
        if (n == 0u) {
            return respond_status(resp, ANV_BL_CMD_WRITE, ANV_BL_ST_OK);
        }
    }

    if (!anv_bl_flash_is_erased(f, offset, n)) {
        return respond_status(resp, ANV_BL_CMD_WRITE, ANV_BL_ST_NOT_ERASED);
    }
    c->verified = false;
    touch(c, offset + n);
    return respond_status(resp, ANV_BL_CMD_WRITE, program_and_verify(c, offset, data, n));
}

static anv_bl_app_header_t stashed_header(const anv_bl_cmd_t *c)
{
    anv_bl_app_header_t hdr;
    anv_bl_header_decode(c->header_raw, &hdr);
    return hdr;
}

static size_t handle_verify(anv_bl_cmd_t *c, const uint8_t *req, size_t len, uint8_t *resp)
{
    if (len != 5u) {
        return respond_status(resp, ANV_BL_CMD_VERIFY, ANV_BL_ST_BAD_LENGTH);
    }
    if (!c->have_header) {
        return respond_status(resp, ANV_BL_CMD_VERIFY, ANV_BL_ST_BAD_SEQUENCE);
    }
    uint32_t expected = rd32(&req[1]);
    anv_bl_app_header_t hdr = stashed_header(c);
    uint32_t actual = anv_bl_flash_crc32(c->flash, ANV_BL_HEADER_SLOT, hdr.image_length);

    c->verified = (actual == expected) && (actual == hdr.image_crc32);
    size_t n = respond_status(resp, ANV_BL_CMD_VERIFY, (uint8_t)(c->verified ? ANV_BL_ST_OK : ANV_BL_ST_MISMATCH));
    wr32(&resp[n], actual);
    return n + 4u;
}

static size_t handle_commit(anv_bl_cmd_t *c, size_t len, uint8_t *resp)
{
    if (len != 1u) {
        return respond_status(resp, ANV_BL_CMD_COMMIT, ANV_BL_ST_BAD_LENGTH);
    }
    if (!c->verified) {
        return respond_status(resp, ANV_BL_CMD_COMMIT, ANV_BL_ST_BAD_SEQUENCE);
    }
    anv_bl_app_header_t hdr = stashed_header(c);
    uint16_t hdr_err = anv_bl_header_check(&hdr, c->node_type, c->flash->region_size);
    if (hdr_err != 0u) {
        return respond_status(resp, ANV_BL_CMD_COMMIT,
                              (hdr_err == ROVER_ERR_WRONG_NODE_TYPE) ? ANV_BL_ST_WRONG_NODE_TYPE : ANV_BL_ST_BAD_IMAGE);
    }
    if (!anv_bl_flash_is_erased(c->flash, 0u, ANV_BL_HEADER_LEN)) {
        return respond_status(resp, ANV_BL_CMD_COMMIT, ANV_BL_ST_NOT_ERASED);
    }
    uint8_t st = program_and_verify(c, 0u, c->header_raw, ANV_BL_HEADER_LEN);
    if (st != ANV_BL_ST_OK) {
        return respond_status(resp, ANV_BL_CMD_COMMIT, st);
    }
    /* Final word: the same check the next boot will make. */
    uint16_t err = anv_bl_app_check(c->flash, c->node_type, NULL);
    c->verified = false;
    if (err != 0u) {
        return respond(resp, ANV_BL_CMD_COMMIT, ANV_BL_ST_BAD_IMAGE, err);
    }
    return respond_status(resp, ANV_BL_CMD_COMMIT, ANV_BL_ST_OK);
}

static size_t handle_boot(anv_bl_cmd_t *c, uint8_t *resp)
{
    uint16_t err = anv_bl_app_check(c->flash, c->node_type, NULL);
    if (err != 0u) {
        return respond(resp, ANV_BL_CMD_BOOT, ANV_BL_ST_BAD_IMAGE, err);
    }
    c->boot_requested = true;
    return respond_status(resp, ANV_BL_CMD_BOOT, ANV_BL_ST_OK);
}

static size_t handle_abort(anv_bl_cmd_t *c, uint8_t *resp)
{
    uint8_t st = ANV_BL_ST_OK;
    /* Only what this session touched: aborting before any erase/write leaves a valid
     * application alone. Erasing from 0 always takes the header with it. */
    if (c->touched_end > 0u &&
        anv_bl_flash_erase_range(c->flash, 0u, c->touched_end, c->feed_watchdog, c->feed_ctx) != 0) {
        st = ANV_BL_ST_FLASH_ERROR;
    }
    reset_session(c);
    return respond_status(resp, ANV_BL_CMD_ABORT, st);
}

static size_t handle_select(anv_bl_cmd_t *c, const uint8_t *req, size_t len, uint8_t *resp)
{
    if (len != 5u) {
        return 0u;
    }
    c->selected = (rd32(&req[1]) == c->uid32);
    if (!c->selected) {
        return 0u;
    }
    size_t n = respond_status(resp, ANV_BL_CMD_SELECT, ANV_BL_ST_OK);
    resp[n] = c->node_id;
    wr32(&resp[n + 1u], c->uid32);
    return n + 5u;
}

size_t anv_bl_cmd_handle(anv_bl_cmd_t *c, const uint8_t *req, size_t req_len, uint8_t *resp)
{
    if (req_len == 0u) {
        return 0u;
    }
    uint8_t cmd = req[0];
    switch (cmd) {
    case ANV_BL_CMD_INFO:
        return handle_info(c, resp);
    case ANV_BL_CMD_ERASE:
        return handle_erase(c, req, req_len, resp);
    case ANV_BL_CMD_WRITE:
        return handle_write(c, req, req_len, resp);
    case ANV_BL_CMD_VERIFY:
        return handle_verify(c, req, req_len, resp);
    case ANV_BL_CMD_COMMIT:
        return handle_commit(c, req_len, resp);
    case ANV_BL_CMD_BOOT:
        return handle_boot(c, resp);
    case ANV_BL_CMD_ABORT:
        return handle_abort(c, resp);
    case ANV_BL_CMD_SELECT:
        return handle_select(c, req, req_len, resp);
    default:
        return respond_status(resp, cmd, ANV_BL_ST_UNKNOWN_COMMAND);
    }
}
