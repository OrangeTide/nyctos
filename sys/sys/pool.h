/*
 * SPDX-License-Identifier: 0BSD OR Apache-2.0
 * Copyright (C) 2026 Jon Mayo <jon@rm-f.net>
 */

/*
 * Static object pools, and the identifiers that name their slots.
 *
 * A pool is a fixed array of same-sized objects with a free list threaded
 * through it. Allocation pops the free list and returns an identifier;
 * closing pushes the slot back and increments its generation, so an
 * identifier held across a free names a valid index whose generation no
 * longer matches and is rejected rather than silently naming the new
 * occupant. See design.md sections 7.1 and 8.
 *
 * An identifier is one pointer-width word: a slot index in the low bits
 * and a generation above it. Generations start at 1, so an all-zero
 * identifier is never valid and zeroed memory does not name slot 0.
 *
 * The split differs by word size because the pools do. Twenty bits of
 * index is a million slots, which is right for a system whose pools are
 * sized in thousands and absurd for a part holding a few dozen objects in
 * total: it would spend half the word describing a range no configuration
 * of that part could reach. The narrower split spends those bits on
 * generation instead, where they are what makes a stale identifier safe.
 *
 * Both splits leave far more generation than the hazard needs. Aliasing
 * requires one slot to be recycled through the whole generation range
 * while a stale identifier naming it is still being held, which is four
 * million reuses in the narrow case.
 */

#ifndef _SYS_POOL_H_
#define _SYS_POOL_H_

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef uintptr_t sys_id_t;

#define SYS_ID_INVALID          ((sys_id_t)0)

#if UINTPTR_MAX > 0xffffffffu
#define SYS_ID_INDEX_BITS       20
#define SYS_ID_GEN_BITS         44
#else
#define SYS_ID_INDEX_BITS       10
#define SYS_ID_GEN_BITS         22
#endif

_Static_assert(SYS_ID_INDEX_BITS + SYS_ID_GEN_BITS == sizeof(sys_id_t) * 8,
  "the index and generation must together fill an identifier exactly");

#define SYS_ID_INDEX_MASK       ((sys_id_t)(((sys_id_t)1 << SYS_ID_INDEX_BITS) - 1))
#define SYS_ID_GEN_MASK         ((sys_id_t)(((sys_id_t)1 << SYS_ID_GEN_BITS) - 1))

/* One more than the largest index a pool may hold. */
#define SYS_POOL_MAX_SLOTS      ((uint32_t)1 << SYS_ID_INDEX_BITS)

/* Free-list terminator. Not a valid slot index, since a pool may never hold
 * SYS_POOL_MAX_SLOTS entries. */
#define SYS_POOL_NIL            ((uint32_t)SYS_POOL_MAX_SLOTS)

static inline uint32_t
sys_id_index(sys_id_t id)
{
    return (uint32_t)(id & SYS_ID_INDEX_MASK);
}

static inline sys_id_t
sys_id_gen(sys_id_t id)
{
    return (id >> SYS_ID_INDEX_BITS) & SYS_ID_GEN_MASK;
}

static inline sys_id_t
sys_id_make(uint32_t index, sys_id_t gen)
{
    return ((gen & SYS_ID_GEN_MASK) << SYS_ID_INDEX_BITS) |
      ((sys_id_t)index & SYS_ID_INDEX_MASK);
}

/*
 * Per-slot bookkeeping, held apart from the objects so that an object type
 * carries no pool machinery of its own.
 */
struct pool_slot {
    sys_id_t gen;               /* generation, masked to SYS_ID_GEN_BITS */
    uint32_t next;              /* next free slot, or SYS_POOL_NIL */
    bool     inuse;
};

struct pool {
    const char       *name;
    uint8_t          *storage;
    struct pool_slot *slots;
    uint32_t          count;
    uint32_t          elem_size;
    uint32_t          free_head;
    uint32_t          used;
    uint32_t          highwater; /* largest 'used' ever reached */
};

/*
 * Declare the backing storage for a pool. The object type and the count are
 * both fixed at compile time, which is the whole point: the memory is in
 * .bss and its size is visible in the link map.
 */
#define POOL_STORAGE(var, type, count)                                  \
    static type var##_objects[count];                                   \
    static struct pool_slot var##_slots[count]

#define POOL_INIT(p, var, name, count)                                  \
    pool_init((p), (name), var##_objects, var##_slots, (count),         \
      (uint32_t)sizeof(var##_objects[0]))

void pool_init(struct pool *p, const char *name, void *objects,
  struct pool_slot *slots, uint32_t count, uint32_t elem_size);

/*
 * Allocate a slot. Returns a pointer to the zeroed object and writes its
 * identifier to out_id, or returns NULL and writes SYS_ID_INVALID when the
 * pool is exhausted. Exhaustion is a configuration error, not a condition to
 * recover from; see design.md section 8.
 */
void *pool_alloc(struct pool *p, sys_id_t *out_id);

/*
 * Release the slot named by id. Returns false if id does not name a live
 * slot, which covers both a stale identifier and a double free.
 */
bool pool_free(struct pool *p, sys_id_t id);

/* Resolve an identifier, or NULL if it is stale, freed, or malformed. */
void *pool_lookup(const struct pool *p, sys_id_t id);

/* Identifier of a live object, given the pointer pool_alloc returned. */
sys_id_t pool_id_of(const struct pool *p, const void *obj);

/*
 * Forget the high-water mark, so a later measurement reports what one
 * stretch of work cost rather than the largest the pool has ever been.
 * The mark is otherwise monotonic, which makes two measurements in a row
 * report the first one twice.
 */
void pool_highwater_reset(struct pool *p);

/* Identifier of the object in slot index, or SYS_ID_INVALID if it is free. */
sys_id_t pool_id_at(const struct pool *p, uint32_t index);

#endif /* _SYS_POOL_H_ */
