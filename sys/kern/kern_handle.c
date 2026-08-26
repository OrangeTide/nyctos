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
handle_table_init(struct handle_table *ht, struct proc *owner)
{
    uint32_t i;

    ht->ht_proc = owner;
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
 * The rights a live handle carries.
 *
 * Almost every caller wants handle_lookup, which asks whether a handle
 * permits something and answers yes or no. This is for the one shape
 * that is different: a grant, where the rights are not only the
 * permission to map but the permission the mapping ends up with, so the
 * caller has to read them rather than assert them.
 */
sys_err_t
handle_rights(const struct handle_table *ht, sys_handle_t h,
  uint32_t *out_rights)
{
    uint32_t index = slot_index(ht, h);

    if (index == SYS_POOL_NIL) {
        return SYS_ERR_STALE;
    }

    *out_rights = ht->ht_slots[index].h_rights;

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
    uint32_t rights;
    uint32_t type;
    void *obj;

    if (index == SYS_POOL_NIL) {
        return SYS_ERR_STALE;
    }
    s = &ht->ht_slots[index];

    /*
     * What the object needs to be told, captured before the slot holding it
     * is taken apart.
     */
    type = s->h_type;
    obj = s->h_object;
    rights = s->h_rights;

    s->h_gen = (s->h_gen + 1) & SYS_ID_GEN_MASK;
    if (s->h_gen == 0) {
        s->h_gen = 1;
    }

    s->h_object = NULL;
    s->h_type = SYS_OBJ_NONE;
    s->h_rights = 0;
    s->h_inuse = false;
    s->h_next = ht->ht_free;
    ht->ht_free = index;
    ht->ht_used--;

    /*
     * Told last, and deliberately. Releasing the last signal right on a
     * timeline wakes its waiters, and waking a waiter can switch straight
     * to it, so this call can return in another thread. Everything that
     * thread might see of this table has to be finished first, rather than
     * merely unreachable because no live handle names the slot.
     */
    handle_object_release(ht, type, obj, rights);

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

sys_err_t
handle_move(struct handle_table *from, struct handle_table *to,
  sys_handle_t h, sys_handle_t *out)
{
    const struct handle_slot *s;
    uint32_t index = slot_index(from, h);
    sys_err_t err;

    if (index == SYS_POOL_NIL) {
        return SYS_ERR_STALE;
    }
    s = &from->ht_slots[index];

    if ((s->h_rights & SYS_RIGHT_TRANSFER) == 0) {
        return SYS_ERR_PERM_DENIED;
    }

    err = handle_insert(to, s->h_object, s->h_type, s->h_rights, out);
    if (err != SYS_OK) {
        return err;
    }

    /*
     * Closed after the insert, so a failure to place it leaves the sender
     * holding what it had rather than losing it to a full table.
     */
    return handle_close(from, h);
}

/*
 * Close everything left in a table, for a process being reaped.
 *
 * Section 7 makes a handle the authority, so a table that outlived its
 * process would be authority that outlived it. Every one goes through
 * handle_close(), which is what tells the object: a timeline losing its
 * last signal right enters the error state and releases its waiters, a
 * grant gives back its mapping, and a process reference dropping to
 * nothing reaps that process too.
 *
 * That last one is why this walks by index and rereads the table each
 * time rather than taking a copy of what to close. Releasing one handle
 * can run arbitrary teardown, including teardown that closes handles,
 * and a list gathered up front would name slots that were already gone.
 */
void
handle_table_close_all(struct handle_table *ht)
{
    uint32_t i;

    for (i = 0; i < SYS_MAX_HANDLES_PER_PROC; i++) {
        if (ht->ht_slots[i].h_inuse) {
            (void)handle_close(ht,
              sys_id_make(i, ht->ht_slots[i].h_gen));
        }
    }

    if (ht->ht_used != 0) {
        panic("handles: %u left in a table being taken apart",
          (unsigned int)ht->ht_used);
    }
}
