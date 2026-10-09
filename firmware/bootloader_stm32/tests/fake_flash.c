#include "fake_flash.h"

#include <string.h>

static int ff_erase_unit(void *ctx, uint32_t index)
{
    fake_flash_t *ff = (fake_flash_t *)ctx;
    uint32_t off;
    uint32_t size;
    if (!anv_bl_flash_unit(&ff->flash, index, &off, &size)) {
        return -1;
    }
    memset(&ff->mem[off], 0xFF, size);
    ff->erase_calls++;
    return 0;
}

static int ff_program(void *ctx, uint32_t offset, const uint8_t *data, uint32_t len)
{
    fake_flash_t *ff = (fake_flash_t *)ctx;
    ff->program_calls++;
    if (ff->fail_program || (offset % ff->flash.write_align) != 0u || (len % ff->flash.write_align) != 0u ||
        offset + len > ff->flash.region_size) {
        return -1;
    }
    for (uint32_t i = 0; i < len; i++) {
        if (ff->mem[offset + i] != 0xFFu) {
            return -1;
        }
        ff->mem[offset + i] &= data[i];
    }
    return 0;
}

static void ff_read(void *ctx, uint32_t offset, uint8_t *out, uint32_t len)
{
    fake_flash_t *ff = (fake_flash_t *)ctx;
    memcpy(out, &ff->mem[offset], len);
}

static void ff_common(fake_flash_t *ff, uint32_t base, uint8_t align, uint8_t n_groups)
{
    ff->flash.ctx = ff;
    ff->flash.base_addr = base;
    ff->flash.write_align = align;
    ff->flash.groups = ff->groups;
    ff->flash.n_groups = n_groups;
    ff->flash.erase_unit = ff_erase_unit;
    ff->flash.program = ff_program;
    ff->flash.read = ff_read;
    ff->flash.region_size = 0u;
    for (uint8_t g = 0; g < n_groups; g++) {
        ff->flash.region_size += (uint32_t)ff->groups[g].count * ff->groups[g].size;
    }
    memset(ff->mem, 0xFF, sizeof(ff->mem));
    ff->erase_calls = 0u;
    ff->program_calls = 0u;
    ff->fail_program = false;
}

void fake_flash_init_g4(fake_flash_t *ff)
{
    ff->groups[0].count = 46u;
    ff->groups[0].size = 2048u;
    ff_common(ff, 0x08009000u, 8u, 1u);
}

void fake_flash_init_f4(fake_flash_t *ff)
{
    ff->groups[0].count = 1u;
    ff->groups[0].size = 64u * 1024u;
    ff->groups[1].count = 3u;
    ff->groups[1].size = 128u * 1024u;
    ff_common(ff, 0x08010000u, 4u, 2u);
}
