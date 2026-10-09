/* The reserved-flash slot log (board_log_flash.c) against a RAM "flash" that behaves like the
 * real thing: programming can only clear bits, and erase is the only way back to 0xFF. */
#include "minitest.h"

#include <string.h>

#include "board_log_flash.h"

#define REGION_SLOTS 4u

typedef struct {
    uint8_t mem[REGION_SLOTS * BOARD_LOG_SLOT_BYTES];
    int erases;
    int programs;
    int fail_after_programs; /* simulate a power cut: programming stops working after N calls (-1 = never) */
} fake_flash_t;

static int ff_erase(void *ctx)
{
    fake_flash_t *f = (fake_flash_t *)ctx;
    memset(f->mem, 0xFF, sizeof(f->mem));
    f->erases++;
    return 0;
}

static int ff_program(void *ctx, uint32_t offset, const uint8_t *data, size_t len)
{
    fake_flash_t *f = (fake_flash_t *)ctx;
    if (f->fail_after_programs >= 0 && f->programs >= f->fail_after_programs) {
        return -1;
    }
    f->programs++;
    for (size_t i = 0; i < len; i++) {
        f->mem[offset + i] &= data[i]; /* flash can only clear bits */
    }
    return 0;
}

static void flash_init(fake_flash_t *ff, board_log_flash_t *f)
{
    memset(ff->mem, 0xFF, sizeof(ff->mem));
    ff->erases = ff->programs = 0;
    ff->fail_after_programs = -1;
    f->mem = ff->mem;
    f->size = sizeof(ff->mem);
    f->ctx = ff;
    f->erase = ff_erase;
    f->program = ff_program;
}

static void test_blank_flash_has_nothing_to_load(void)
{
    fake_flash_t ff;
    board_log_flash_t f;
    flash_init(&ff, &f);
    uint8_t buf[400];
    MT_CHECK(board_log_flash_load(&f, buf, sizeof(buf)) != 0);
}

static void test_load_returns_the_newest_save(void)
{
    fake_flash_t ff;
    board_log_flash_t f;
    flash_init(&ff, &f);
    uint8_t blob[400], out[400];
    for (uint8_t v = 1; v <= 3; v++) {
        memset(blob, v, sizeof(blob));
        MT_CHECK(board_log_flash_save(&f, blob, sizeof(blob)) == 0);
    }
    MT_CHECK(board_log_flash_load(&f, out, sizeof(out)) == 0);
    MT_CHECK(out[0] == 3u && out[399] == 3u);
    MT_CHECK(ff.erases == 0); /* three saves, three slots, no erase */
}

static void test_erase_only_when_the_region_is_full(void)
{
    fake_flash_t ff;
    board_log_flash_t f;
    flash_init(&ff, &f);
    uint8_t blob[400], out[400];
    for (uint8_t v = 1; v <= REGION_SLOTS; v++) {
        memset(blob, v, sizeof(blob));
        MT_CHECK(board_log_flash_save(&f, blob, sizeof(blob)) == 0);
    }
    MT_CHECK(ff.erases == 0);

    memset(blob, 9, sizeof(blob));
    MT_CHECK(board_log_flash_save(&f, blob, sizeof(blob)) == 0); /* full: erase, start over */
    MT_CHECK(ff.erases == 1);
    MT_CHECK(board_log_flash_load(&f, out, sizeof(out)) == 0 && out[0] == 9u);
}

static void test_power_cut_during_a_save_keeps_the_previous_log(void)
{
    fake_flash_t ff;
    board_log_flash_t f;
    flash_init(&ff, &f);
    uint8_t blob[400], out[400];
    memset(blob, 1, sizeof(blob));
    MT_CHECK(board_log_flash_save(&f, blob, sizeof(blob)) == 0);

    /* The body is programmed but the commit (first word) never happens. */
    memset(blob, 2, sizeof(blob));
    ff.fail_after_programs = ff.programs + 1;
    MT_CHECK(board_log_flash_save(&f, blob, sizeof(blob)) != 0);

    MT_CHECK(board_log_flash_load(&f, out, sizeof(out)) == 0);
    MT_CHECK(out[0] == 1u); /* still the old one */

    /* The torn slot is skipped by the next save, which lands in a clean slot. */
    ff.fail_after_programs = -1;
    memset(blob, 3, sizeof(blob));
    MT_CHECK(board_log_flash_save(&f, blob, sizeof(blob)) == 0);
    MT_CHECK(board_log_flash_load(&f, out, sizeof(out)) == 0 && out[0] == 3u && out[399] == 3u);
}

static void test_oversized_blob_is_refused(void)
{
    fake_flash_t ff;
    board_log_flash_t f;
    flash_init(&ff, &f);
    uint8_t big[BOARD_LOG_SLOT_BYTES + 8];
    memset(big, 0, sizeof(big));
    MT_CHECK(board_log_flash_save(&f, big, sizeof(big)) != 0);
    MT_CHECK(board_log_flash_load(&f, big, sizeof(big)) != 0);
}

int main(void)
{
    MT_RUN(test_blank_flash_has_nothing_to_load);
    MT_RUN(test_load_returns_the_newest_save);
    MT_RUN(test_erase_only_when_the_region_is_full);
    MT_RUN(test_power_cut_during_a_save_keeps_the_previous_log);
    MT_RUN(test_oversized_blob_is_refused);
    return mt_summary();
}
