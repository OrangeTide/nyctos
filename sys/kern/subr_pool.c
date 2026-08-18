/*
 * SPDX-License-Identifier: 0BSD OR Apache-2.0
 * Copyright (C) 2026 Jon Mayo <jon@rm-f.net>
 */

/*
 * Static object pools. See sys/sys/pool.h for the identifier layout and the
 * reasoning; this file is the mechanism and holds no policy.
 */

#include <sys/pool.h>
#include <sys/systm.h>

static void *
slot_object(const struct pool *p, uint32_t index)
{
    return p->storage + ((size_t)index * p->elem_size);
}

/*
 * Build the free list back to front so that the first allocation returns
 * slot 0. Nothing depends on that, but a predictable layout makes a boot
 * log far easier to read.
 */
void
pool_init(struct pool *p, const char *name, void *objects,
  struct pool_slot *slots, uint32_t count, uint32_t elem_size)
{
    uint32_t i;

    if (count == 0 || count >= SYS_POOL_MAX_SLOTS)
        panic("pool %s: bad count %u", name, count);

    p->name = name;
    p->storage = objects;
    p->slots = slots;
    p->count = count;
    p->elem_size = elem_size;
    p->used = 0;
    p->highwater = 0;
    p->free_head = SYS_POOL_NIL;

    for (i = count; i > 0; i--) {
        slots[i - 1].gen = 1;
        slots[i - 1].inuse = false;
        slots[i - 1].next = p->free_head;
        p->free_head = i - 1;
    }
}

void *
pool_alloc(struct pool *p, sys_id_t *out_id)
{
    struct pool_slot *s;
    uint32_t index;
    void *obj;

    if (p->free_head == SYS_POOL_NIL) {
        if (out_id != NULL)
            *out_id = SYS_ID_INVALID;
        return NULL;
    }

    index = p->free_head;
    s = &p->slots[index];
    p->free_head = s->next;

    s->next = SYS_POOL_NIL;
    s->inuse = true;

    p->used++;
    if (p->used > p->highwater)
        p->highwater = p->used;

    obj = slot_object(p, index);
    memset(obj, 0, p->elem_size);

    if (out_id != NULL)
        *out_id = sys_id_make(index, s->gen);

    return obj;
}

/*
 * The generation is bumped on free rather than on allocation, so that every
 * identifier handed out for this slot so far becomes stale at exactly the
 * moment the object stops existing.
 */
bool
pool_free(struct pool *p, sys_id_t id)
{
    struct pool_slot *s;
    uint32_t index;

    index = sys_id_index(id);
    if (index >= p->count)
        return false;

    s = &p->slots[index];
    if (!s->inuse || s->gen != sys_id_gen(id))
        return false;

    s->gen = (s->gen + 1) & SYS_ID_GEN_MASK;
    if (s->gen == 0)
        s->gen = 1;

    s->inuse = false;
    s->next = p->free_head;
    p->free_head = index;
    p->used--;

    return true;
}

void *
pool_lookup(const struct pool *p, sys_id_t id)
{
    const struct pool_slot *s;
    uint32_t index;

    index = sys_id_index(id);
    if (index >= p->count)
        return NULL;

    s = &p->slots[index];
    if (!s->inuse || s->gen != sys_id_gen(id))
        return NULL;

    return slot_object(p, index);
}

sys_id_t
pool_id_of(const struct pool *p, const void *obj)
{
    size_t offset;
    uint32_t index;

    offset = (size_t)((const uint8_t *)obj - p->storage);
    index = (uint32_t)(offset / p->elem_size);

    if (index >= p->count || !p->slots[index].inuse)
        return SYS_ID_INVALID;

    return sys_id_make(index, p->slots[index].gen);
}

sys_id_t
pool_id_at(const struct pool *p, uint32_t index)
{
    if (index >= p->count || !p->slots[index].inuse)
        return SYS_ID_INVALID;

    return sys_id_make(index, p->slots[index].gen);
}
