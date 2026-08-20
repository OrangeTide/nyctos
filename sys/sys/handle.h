/*
 * SPDX-License-Identifier: 0BSD OR Apache-2.0
 * Copyright (C) 2026 Jon Mayo <jon@rm-f.net>
 */

/*
 * Per-process handle tables, per section 7.
 *
 * A handle is authority. What a process can reach is exactly what its table
 * contains, so this is the only place a reference to a kernel object is
 * turned into permission to act on it.
 *
 * Handle values carry the same index and generation split as pool
 * identifiers, so a handle held across a close names a valid index whose
 * generation no longer matches and is rejected rather than reaching
 * whatever now occupies the slot.
 *
 * A handle value means nothing outside the process holding it. Two
 * processes may hold the same integer and reach different objects. Nothing
 * here may be used as a system-wide identity.
 */

#ifndef _SYS_HANDLE_H_
#define _SYS_HANDLE_H_

#include <sys/config.h>
#include <sys/op.h>
#include <sys/pool.h>

#include <stdbool.h>
#include <stdint.h>

struct handle_slot {
    void       *h_object;
    sys_id_t    h_gen;
    uint32_t    h_type;         /* SYS_OBJ_* */
    uint32_t    h_rights;
    uint32_t    h_next;         /* free list link, or SYS_POOL_NIL */
    bool        h_inuse;
};

struct handle_table {
    struct handle_slot ht_slots[SYS_MAX_HANDLES_PER_PROC];
    uint32_t           ht_free;
    uint32_t           ht_used;
};

/*
 * Called when a handle is created or destroyed, so an object that cares who
 * still holds authority over it can find out. A timeline that loses its
 * last signal-righted handle enters the error state, per section 9.3.
 */
void handle_object_retain(uint32_t type, void *obj, uint32_t rights);
void handle_object_release(uint32_t type, void *obj, uint32_t rights);

void handle_table_init(struct handle_table *ht);

/*
 * Put an object in the table and return a handle naming it. The kernel
 * chooses the slot arbitrarily: section 7.3 keeps lowest-first allocation
 * out of the kernel, because it serialises a hot path and makes handle
 * values predictable.
 */
sys_err_t handle_insert(struct handle_table *ht, void *obj, uint32_t type,
  uint32_t rights, sys_handle_t *out);

/*
 * Resolve a handle, checking the type and that it carries every right in
 * need_rights. Pass SYS_OBJ_NONE to accept any type and 0 to require no
 * particular right. Fails without doing anything: SYS_ERR_STALE for a
 * handle that names no live slot, SYS_ERR_WRONG_TYPE for the wrong object,
 * SYS_ERR_PERM_DENIED for a missing right.
 */
sys_err_t handle_lookup(const struct handle_table *ht, sys_handle_t h,
  uint32_t type, uint32_t need_rights, void **out_obj);

sys_err_t handle_close(struct handle_table *ht, sys_handle_t h);

/*
 * Move a handle from one table to another, per section 7.4. The value
 * changes, the source slot is invalidated, and the rights travel
 * unchanged: attenuation happens before a transfer, not during one, so a
 * sender that wants to give less duplicates with fewer rights first.
 */
sys_err_t handle_move(struct handle_table *from, struct handle_table *to,
  sys_handle_t h, sys_handle_t *out);

/*
 * Duplicate, attenuating. new_rights must be a subset of what the source
 * carries, and the source must carry SYS_RIGHT_DUPLICATE.
 */
sys_err_t handle_dup(struct handle_table *ht, sys_handle_t h,
  uint32_t new_rights, sys_handle_t *out);

#endif /* _SYS_HANDLE_H_ */
