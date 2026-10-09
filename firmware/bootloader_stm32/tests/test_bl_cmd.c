/* Command logic on its own (no CAN, no ISO-TP): every command against a fake flash, for both
 * flash layouts in docs/architecture.md. */
#include <stdio.h>
#include <string.h>

#include "anv_bl_cmd.h"
#include "anv_bl_image.h"
#include "errors.h"
#include "fake_flash.h"
#include "minitest.h"
#include "test_image.h"

#define NODE_TYPE_DRIVE 1u

static fake_flash_t s_ff;
static anv_bl_cmd_t s_cmd;
static test_image_t s_img;
static uint8_t s_req[ANV_BL_MAX_REQUEST];
static uint8_t s_resp[ANV_BL_MAX_RESPONSE];
static size_t s_resp_len;

static const uint8_t k_uid[12] = {1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12};

static void put32(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)v;
    p[1] = (uint8_t)(v >> 8);
    p[2] = (uint8_t)(v >> 16);
    p[3] = (uint8_t)(v >> 24);
}

static uint32_t get32(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static uint16_t resp_error(void)
{
    return (uint16_t)(s_resp[2] | (s_resp[3] << 8));
}

static uint8_t do_req(size_t len)
{
    s_resp_len = anv_bl_cmd_handle(&s_cmd, s_req, len, s_resp);
    return (s_resp_len >= ANV_BL_RESP_PREFIX_LEN) ? s_resp[1] : 0xEEu;
}

static uint8_t cmd0(uint8_t cmd)
{
    s_req[0] = cmd;
    return do_req(1u);
}

static uint8_t erase(uint32_t off, uint32_t len)
{
    s_req[0] = ANV_BL_CMD_ERASE;
    put32(&s_req[1], off);
    put32(&s_req[5], len);
    return do_req(9u);
}

static uint8_t write_at(uint32_t off, const uint8_t *data, uint32_t len)
{
    s_req[0] = ANV_BL_CMD_WRITE;
    put32(&s_req[1], off);
    memcpy(&s_req[5], data, len);
    return do_req(5u + len);
}

static uint8_t verify(uint32_t crc)
{
    s_req[0] = ANV_BL_CMD_VERIFY;
    put32(&s_req[1], crc);
    return do_req(5u);
}

static uint8_t write_image(const test_image_t *img, uint32_t block)
{
    for (uint32_t off = 0; off < img->len; off += block) {
        uint32_t n = (img->len - off < block) ? img->len - off : block;
        uint8_t st = write_at(off, &img->bytes[off], n);
        if (st != ANV_BL_ST_OK) {
            return st;
        }
    }
    return ANV_BL_ST_OK;
}

static void setup_g4(void)
{
    fake_flash_init_g4(&s_ff);
    anv_bl_cmd_init(&s_cmd, &s_ff.flash, 4u, NODE_TYPE_DRIVE, k_uid, 128u);
}

static void test_full_sequence_g4(void)
{
    setup_g4();
    test_image_build(&s_img, NODE_TYPE_DRIVE, 10001u, 1u, 0xCAFE1234u); /* odd length: tail pad */

    MT_CHECK(erase(0u, s_img.len) == ANV_BL_ST_OK);
    MT_CHECK(s_ff.erase_calls == (s_img.len + 2047u) / 2048u);
    MT_CHECK(write_image(&s_img, ANV_BL_MAX_WRITE) == ANV_BL_ST_OK);
    /* Header slot untouched until COMMIT. */
    MT_CHECK(anv_bl_flash_is_erased(&s_ff.flash, 0u, ANV_BL_HEADER_SLOT));
    MT_CHECK(memcmp(&s_ff.mem[ANV_BL_HEADER_SLOT], &s_img.bytes[ANV_BL_HEADER_SLOT], 10001u) == 0);
    MT_CHECK(s_ff.mem[ANV_BL_HEADER_SLOT + 10001u] == 0xFFu); /* padding */
    MT_CHECK(anv_bl_app_check(&s_ff.flash, NODE_TYPE_DRIVE, NULL) == ROVER_ERR_APP_CRC_FAIL);

    MT_CHECK(verify(s_img.hdr.image_crc32) == ANV_BL_ST_OK);
    MT_CHECK(get32(&s_resp[4]) == s_img.hdr.image_crc32);
    MT_CHECK(cmd0(ANV_BL_CMD_COMMIT) == ANV_BL_ST_OK);
    MT_CHECK(memcmp(s_ff.mem, s_img.bytes, ANV_BL_HEADER_LEN) == 0);
    anv_bl_app_header_t hdr;
    MT_CHECK(anv_bl_app_check(&s_ff.flash, NODE_TYPE_DRIVE, &hdr) == 0u);
    MT_CHECK(hdr.build_id == 0xCAFE1234u);

    MT_CHECK(cmd0(ANV_BL_CMD_BOOT) == ANV_BL_ST_OK);
    MT_CHECK(s_cmd.boot_requested);
}

static void test_full_sequence_f4_sectors(void)
{
    fake_flash_init_f4(&s_ff);
    anv_bl_cmd_init(&s_cmd, &s_ff.flash, 5u, NODE_TYPE_DRIVE, k_uid, 512u);
    test_image_build(&s_img, NODE_TYPE_DRIVE, 64u * 1024u - ANV_BL_HEADER_SLOT + 100u, 2u, 7u);

    /* Spills 100 bytes past the 64 KB sector: two sectors erased. */
    MT_CHECK(erase(0u, s_img.len) == ANV_BL_ST_OK);
    MT_CHECK(s_ff.erase_calls == 2u);
    MT_CHECK(write_image(&s_img, 2048u) == ANV_BL_ST_OK);
    MT_CHECK(verify(s_img.hdr.image_crc32) == ANV_BL_ST_OK);
    MT_CHECK(cmd0(ANV_BL_CMD_COMMIT) == ANV_BL_ST_OK);
    MT_CHECK(anv_bl_app_check(&s_ff.flash, NODE_TYPE_DRIVE, NULL) == 0u);
}

static void test_info(void)
{
    setup_g4();
    MT_CHECK(cmd0(ANV_BL_CMD_INFO) == ANV_BL_ST_OK);
    const uint8_t *p = &s_resp[ANV_BL_RESP_PREFIX_LEN];
    MT_CHECK((p[0] | (p[1] << 8)) == ANV_BL_VERSION);
    MT_CHECK(p[2] == 4u && p[3] == NODE_TYPE_DRIVE);
    MT_CHECK(memcmp(&p[4], k_uid, 12u) == 0);
    MT_CHECK(get32(&p[16]) == anv_bl_uid32(k_uid));
    MT_CHECK(get32(&p[22]) == 0x08009000u);
    MT_CHECK(get32(&p[26]) == 46u * 2048u);
    MT_CHECK(p[32] == 8u);
    MT_CHECK((p[35] | (p[36] << 8)) == ROVER_ERR_APP_CRC_FAIL); /* empty flash: no app */
    MT_CHECK(p[37] == 1u);
    MT_CHECK((p[38] | (p[39] << 8)) == 46u && get32(&p[40]) == 2048u);
    MT_CHECK(s_resp_len == ANV_BL_RESP_PREFIX_LEN + ANV_BL_INFO_FIXED_LEN + 6u + ANV_BL_HEADER_LEN);
}

static void test_wrong_node_type_rejected(void)
{
    setup_g4();
    test_image_build(&s_img, 2u /* arm */, 4096u, 3u, 1u);
    MT_CHECK(erase(0u, s_img.len) == ANV_BL_ST_OK);
    MT_CHECK(write_at(0u, s_img.bytes, ANV_BL_MAX_WRITE) == ANV_BL_ST_WRONG_NODE_TYPE);
    MT_CHECK(resp_error() == ROVER_ERR_WRONG_NODE_TYPE);
    MT_CHECK(anv_bl_flash_is_erased(&s_ff.flash, 0u, s_img.len)); /* nothing programmed */
    MT_CHECK(verify(s_img.hdr.image_crc32) == ANV_BL_ST_BAD_SEQUENCE);
    MT_CHECK(cmd0(ANV_BL_CMD_COMMIT) == ANV_BL_ST_BAD_SEQUENCE);

    /* An image of the wrong type already in flash (e.g. put there over SWD) never boots. */
    memcpy(s_ff.mem, s_img.bytes, s_img.len);
    MT_CHECK(anv_bl_app_check(&s_ff.flash, NODE_TYPE_DRIVE, NULL) == ROVER_ERR_WRONG_NODE_TYPE);
    MT_CHECK(cmd0(ANV_BL_CMD_BOOT) == ANV_BL_ST_BAD_IMAGE);
    MT_CHECK(resp_error() == ROVER_ERR_WRONG_NODE_TYPE);
    MT_CHECK(!s_cmd.boot_requested);
}

static void test_crc_mismatch_rejected(void)
{
    setup_g4();
    test_image_build(&s_img, NODE_TYPE_DRIVE, 6000u, 4u, 1u);
    s_img.bytes[ANV_BL_HEADER_SLOT + 5000u] ^= 0x01u; /* corrupted in transit / bad file */

    MT_CHECK(erase(0u, s_img.len) == ANV_BL_ST_OK);
    MT_CHECK(write_image(&s_img, 1024u) == ANV_BL_ST_OK);
    MT_CHECK(verify(s_img.hdr.image_crc32) == ANV_BL_ST_MISMATCH);
    MT_CHECK(resp_error() == ROVER_ERR_APP_CRC_FAIL);
    MT_CHECK(get32(&s_resp[4]) != s_img.hdr.image_crc32);
    MT_CHECK(cmd0(ANV_BL_CMD_COMMIT) == ANV_BL_ST_BAD_SEQUENCE);
    MT_CHECK(anv_bl_flash_is_erased(&s_ff.flash, 0u, ANV_BL_HEADER_LEN)); /* header never written */
    MT_CHECK(cmd0(ANV_BL_CMD_BOOT) == ANV_BL_ST_BAD_IMAGE);

    /* Host and node disagree on the expected CRC: also a mismatch. */
    test_image_build(&s_img, NODE_TYPE_DRIVE, 6000u, 4u, 1u);
    MT_CHECK(erase(0u, s_img.len) == ANV_BL_ST_OK);
    MT_CHECK(write_image(&s_img, 1024u) == ANV_BL_ST_OK);
    MT_CHECK(verify(s_img.hdr.image_crc32 ^ 1u) == ANV_BL_ST_MISMATCH);
    MT_CHECK(verify(s_img.hdr.image_crc32) == ANV_BL_ST_OK);
}

static void test_corrupt_header_rejected(void)
{
    setup_g4();
    test_image_build(&s_img, NODE_TYPE_DRIVE, 3000u, 5u, 1u);
    s_img.bytes[20] ^= 0x40u; /* build_id byte flipped, header CRC no longer matches */
    MT_CHECK(erase(0u, s_img.len) == ANV_BL_ST_OK);
    MT_CHECK(write_at(0u, s_img.bytes, 1024u) == ANV_BL_ST_BAD_IMAGE);
    MT_CHECK(resp_error() == ROVER_ERR_APP_CRC_FAIL);
}

static void test_write_rules(void)
{
    setup_g4();
    test_image_build(&s_img, NODE_TYPE_DRIVE, 8192u, 6u, 1u);
    uint8_t data[16];
    memset(data, 0x55, sizeof(data));

    /* Not erased yet (fresh fake flash is erased, so dirty it first). */
    MT_CHECK(erase(0u, 4096u) == ANV_BL_ST_OK);
    MT_CHECK(write_at(0x400u, data, 16u) == ANV_BL_ST_OK);
    MT_CHECK(write_at(0x400u, data, 16u) == ANV_BL_ST_NOT_ERASED);
    MT_CHECK(resp_error() == ROVER_ERR_FLASH_WRITE_FAIL);
    /* Misaligned, outside the region, header slot without the header, empty. */
    MT_CHECK(write_at(0x404u, data, 8u) == ANV_BL_ST_BAD_RANGE);
    MT_CHECK(write_at(s_ff.flash.region_size - 8u, data, 16u) == ANV_BL_ST_BAD_RANGE);
    MT_CHECK(write_at(0x100u, data, 16u) == ANV_BL_ST_BAD_RANGE);
    MT_CHECK(write_at(0u, s_img.bytes, 32u) == ANV_BL_ST_BAD_RANGE);
    MT_CHECK(write_at(0x800u, data, 0u) == ANV_BL_ST_BAD_LENGTH);
    /* ERASE range checks. */
    MT_CHECK(erase(0u, 0u) == ANV_BL_ST_BAD_RANGE);
    MT_CHECK(erase(s_ff.flash.region_size, 1u) == ANV_BL_ST_BAD_RANGE);
    MT_CHECK(erase(4096u, 0xFFFFFFFFu) == ANV_BL_ST_BAD_RANGE);
    /* Flash failure reported as FLASH_ERROR with FLASH_WRITE_FAIL. */
    s_ff.fail_program = true;
    MT_CHECK(write_at(0x1000u, data, 16u) == ANV_BL_ST_FLASH_ERROR);
    MT_CHECK(resp_error() == ROVER_ERR_FLASH_WRITE_FAIL);
    s_ff.fail_program = false;
    /* Unknown command. */
    MT_CHECK(cmd0(0x7Eu) == ANV_BL_ST_UNKNOWN_COMMAND);
    MT_CHECK(s_resp[0] == 0x7Eu);
}

static void test_abort_erases_partial_image(void)
{
    setup_g4();
    /* A valid application already in flash. */
    test_image_build(&s_img, NODE_TYPE_DRIVE, 5000u, 7u, 1u);
    memcpy(s_ff.mem, s_img.bytes, s_img.len);
    MT_CHECK(anv_bl_app_check(&s_ff.flash, NODE_TYPE_DRIVE, NULL) == 0u);

    /* ABORT before touching anything leaves it alone. */
    MT_CHECK(cmd0(ANV_BL_CMD_ABORT) == ANV_BL_ST_OK);
    MT_CHECK(anv_bl_app_check(&s_ff.flash, NODE_TYPE_DRIVE, NULL) == 0u);

    test_image_build(&s_img, NODE_TYPE_DRIVE, 20000u, 8u, 2u);
    MT_CHECK(erase(0u, s_img.len) == ANV_BL_ST_OK);
    MT_CHECK(write_at(0u, s_img.bytes, 4096u) == ANV_BL_ST_OK);
    MT_CHECK(write_at(4096u, &s_img.bytes[4096], 4096u) == ANV_BL_ST_OK);
    MT_CHECK(cmd0(ANV_BL_CMD_ABORT) == ANV_BL_ST_OK);
    MT_CHECK(anv_bl_flash_is_erased(&s_ff.flash, 0u, s_img.len));
    MT_CHECK(!s_cmd.have_header);
    MT_CHECK(verify(s_img.hdr.image_crc32) == ANV_BL_ST_BAD_SEQUENCE);
}

static void test_select(void)
{
    setup_g4();
    s_req[0] = ANV_BL_CMD_SELECT;
    put32(&s_req[1], anv_bl_uid32(k_uid) ^ 1u);
    MT_CHECK(anv_bl_cmd_handle(&s_cmd, s_req, 5u, s_resp) == 0u); /* another board: silent */
    MT_CHECK(!s_cmd.selected);
    put32(&s_req[1], anv_bl_uid32(k_uid));
    MT_CHECK(do_req(5u) == ANV_BL_ST_OK);
    MT_CHECK(s_cmd.selected);
    MT_CHECK(s_resp[4] == 4u && get32(&s_resp[5]) == anv_bl_uid32(k_uid));
}

static void test_crc32_matches_zlib(void)
{
    /* zlib.crc32(b"123456789") == 0xCBF43926, the standard check value. */
    MT_CHECK(anv_bl_crc32(0u, (const uint8_t *)"123456789", 9u) == 0xCBF43926u);
    MT_CHECK(anv_bl_crc32(anv_bl_crc32(0u, (const uint8_t *)"1234", 4u), (const uint8_t *)"56789", 5u) ==
             0xCBF43926u);
}

int main(void)
{
    MT_RUN(test_crc32_matches_zlib);
    MT_RUN(test_full_sequence_g4);
    MT_RUN(test_full_sequence_f4_sectors);
    MT_RUN(test_info);
    MT_RUN(test_wrong_node_type_rejected);
    MT_RUN(test_crc_mismatch_rejected);
    MT_RUN(test_corrupt_header_rejected);
    MT_RUN(test_write_rules);
    MT_RUN(test_abort_erases_partial_image);
    MT_RUN(test_select);
    return mt_summary();
}
