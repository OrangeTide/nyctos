/*
 * SPDX-License-Identifier: 0BSD OR Apache-2.0
 * Copyright (C) 2026 Jon Mayo <jon@rm-f.net>
 */

/*
 * Handle tables. See sys/sys/handle.h for what a handle is and why the
 * generation is in it.
 */

#include <sys/handle.h>
#include <sys/systm.h>

void
handle_table_init(struct handle_table *ht)
{
    uint32_t i;

    ht->ht_free = SYS_POOL_NIL;
    ht->ht_used = 0;

    for (i = SYS_MAX_HANDLES_PER_PROC; i > 0; i--) {
        struct handle_slot *s = &ht->ht_slots[i - 1];

        s->h_object = NULL;
        s->h_type = SYS_OBJ_NONE;
        s->h_rights = 0;
        s->h_gen = 1;
        s->h_inuse = false;
        s->h_next = ht->ht_free;
        ht->ht_free = i - 1;
    }
}

/*
 * Resolve to a slot index, checking nothing but liveness, which is what
 * close and dup need before they look at rights. Returning the index rather
 * than a pointer lets a caller that only reads keep its table const.
 */
static uint32_t
slot_index(const struct handle_table *ht, sys_handle_t h)
{
    const struct handle_slot *s;
    uint32_t index;

    index = sys_id_index(h);
    if (index >= SYS_MAX_HANDLES_PER_PROC) {
        return SYS_POOL_NIL;
    }

    s = &ht->ht_slots[index];
    if (!s->h_inuse || s->h_gen != sys_id_gen(h)) {
        return SYS_POOL_NIL;
    }

    return index;
}

sys_err_t
handle_insert(struct handle_table *ht, void *obj, uint32_t type,
  uint32_t rights, sys_handle_t *out)
{
    struct handle_slot *s;
    uint32_t index;

    if (obj == NULL || type == SYS_OBJ_NONE) {
        return SYS_ERR_INVALID_ARG;
    }
    if (ht->ht_free == SYS_POOL_NIL) {
        return SYS_ERR_NO_MEMORY;
    }

    index = ht->ht_free;
    s = &ht->ht_slots[index];
    ht->ht_free = s->h_next;

    s->h_object = obj;
    s->h_type = type;
    s->h_rights = rights;
    s->h_next = SYS_POOL_NIL;
    s->h_inuse = true;
    ht->ht_used++;

    handle_object_retain(type, obj, rights);

    *out = sys_id_make(index, s->h_gen);

    return SYS_OK;
}

sys_err_t
handle_lookup(const struct handle_table *ht, sys_handle_t h, uint32_t type,
  uint32_t need_rights, void **out_obj)
{
    const struct handle_slot *s;
    uint32_t index = slot_index(ht, h);

    if (index == SYS_POOL_NIL) {
        return SYS_ERR_STALE;
    }
    s = &ht->ht_slots[index];

    if (type != SYS_OBJ_NONE && s->h_type != type) {
        return SYS_ERR_WRONG_TYPE;
    }
    if ((s->h_rights & need_rights) != need_rights) {
        return SYS_ERR_PERM_DENIED;
    }

    if (out_obj != NULL) {
        *out_obj = s->h_object;
    }

    return SYS_OK;
}

/*
 * The generation advances on close rather than on allocation, so every
 * handle value handed out for this slot goes stale at the moment the
 * authority is given up.
 */
sys_err_t
handle_close(struct handle_table *ht, sys_handle_t h)
{
    struct handle_slot *s;
    uint32_t index = slot_index(ht, h);

    if (index == SYS_POOL_NIL) {
        return SYS_ERR_STALE;
    }
    s = &ht->ht_slots[index];

    s->h_gen = (s->h_gen + 1) & SYS_ID_GEN_MASK;
    if (s->h_gen == 0) {
        s->h_gen = 1;
    }

    /*
     * The object is told before the slot is cleared, and after the
     * generation has moved, so that anything the release triggers sees a
     * table in which this handle is already stale.
     */
    handle_object_release(s->h_type, s->h_object, s->h_rights);

    s->h_object = NULL;
    s->h_type = SYS_OBJ_NONE;
    s->h_rights = 0;
    s->h_inuse = false;
    s->h_next = ht->ht_free;
    ht->ht_free = index;
    ht->ht_used--;

    return SYS_OK;
}

sys_err_t
handle_dup(struct handle_table *ht, sys_handle_t h, uint32_t new_rights,
  sys_handle_t *out)
{
    const struct handle_slot *s;
    uint32_t index = slot_index(ht, h);

    if (index == SYS_POOL_NIL) {
        return SYS_ERR_STALE;
    }
    s = &ht->ht_slots[index];

    if ((s->h_rights & SYS_RIGHT_DUPLICATE) == 0) {
        return SYS_ERR_PERM_DENIED;
    }

    /*
     * Section 7.2: duplication may only attenuate. A request for anything
     * the source lacks is refused rather than quietly clamped, because a
     * caller that asked for more than it holds has made a mistake worth
     * hearing about.
     */
    if ((new_rights & ~s->h_rights) != 0) {
        return SYS_ERR_PERM_DENIED;
    }

    return handle_insert(ht, s->h_object, s->h_type, new_rights, out);
}
