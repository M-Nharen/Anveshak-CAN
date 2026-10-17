/* anv_node storage in RAM: for Linux tests, and for simulated nodes on a host. */
#include <string.h>

#include "anv_node.h"

static anv_node_memstore_slot_t *find(anv_node_memstore_t *s, const char *key)
{
    for (unsigned i = 0; i < ANV_NODE_MEMSTORE_SLOTS; i++) {
        if (s->slots[i].used && strncmp(s->slots[i].key, key, sizeof s->slots[i].key) == 0) {
            return &s->slots[i];
        }
    }
    return NULL;
}

static size_t mem_load(void *ctx, const char *key, void *buf, size_t cap)
{
    anv_node_memstore_slot_t *slot = find((anv_node_memstore_t *)ctx, key);
    if (slot == NULL || slot->len > cap) {
        return 0;
    }
    memcpy(buf, slot->data, slot->len);
    return slot->len;
}

static bool mem_save(void *ctx, const char *key, const void *buf, size_t len)
{
    anv_node_memstore_t *s = (anv_node_memstore_t *)ctx;
    if (s->fail_writes || len > ANV_NODE_MEMSTORE_SIZE || strlen(key) >= sizeof s->slots[0].key) {
        return false;
    }
    anv_node_memstore_slot_t *slot = find(s, key);
    for (unsigned i = 0; slot == NULL && i < ANV_NODE_MEMSTORE_SLOTS; i++) {
        if (!s->slots[i].used) {
            slot = &s->slots[i];
        }
    }
    if (slot == NULL) {
        return false;
    }
    strcpy(slot->key, key);
    memcpy(slot->data, buf, len);
    slot->len = len;
    slot->used = true;
    return true;
}

static bool mem_erase(void *ctx, const char *key)
{
    anv_node_memstore_slot_t *slot = find((anv_node_memstore_t *)ctx, key);
    if (slot != NULL) {
        slot->used = false;
    }
    return true;
}

static const anv_node_storage_t mem_ops = {mem_load, mem_save, mem_erase};

void anv_node_memstore_init(anv_node_memstore_t *store)
{
    memset(store, 0, sizeof *store);
}

const anv_node_storage_t *anv_node_memstore_ops(void)
{
    return &mem_ops;
}
