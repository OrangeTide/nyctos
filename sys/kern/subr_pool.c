/*
 * SPDX-License-Identifier: 0BSD OR Apache-2.0
 * Copyright (C) 2026 Jon Mayo <jon@rm-f.net>
 */

/*
 * Static object pools. See sys/sys/pool.h for the identifier layout and the
 * reasoning; this file is the mechanism and holds no policy.
 */

#include <sys/cpu.h>
#include <sys/lock.h>
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


/*
 * One lock for every pool, and what it is not.
 *
 * A pool is a free list and two counters, and both cores reach them: a
 * thread ending on one core gives its slot back while the other is
 * taking one. Without exclusion the list itself is what breaks, not the
 * counters, and two callers can be handed the same slot.
 *
 * Section 11.4 forbids a global scheduler lock and this is not one.
 * Nothing here decides what runs, and the scheduling decision does not
 * come through here: the dispatch path checks that it takes no lock
 * another core could hold, and reaping a slot happens before that check
 * begins rather than inside it.
 *
 * A leaf, and deliberately. Nothing under this lock takes another, so
 * it cannot be half of a cycle, and holding it with interrupts disabled
 * keeps a handler on this core from arriving at a pool this core is
 * already inside.
 */
#if SYS_TIER == 2

static struct spinlock pool_lock = { 0, 0, "pools" };

/*
 * How many times each core has been in here. Not a measurement of
 * anything the system does, but of whether the lock above is needed:
 * a second core that never allocates would leave every pool number
 * correct for the wrong reason and the lock untested.
 */
static uint64_t pool_ops[SYS_MAX_CORES];

static intr_state_t
pool_enter(void)
{
    intr_state_t s = md_intr_disable();

    spin_lock(&pool_lock);
    pool_ops[cpu_self()->cp_index]++;

    return s;
}

static void
pool_leave(intr_state_t s)
{
    spin_unlock(&pool_lock);
    md_intr_restore(s);
}

#else

/*
 * Section 13.3 buys Tier 0 a kernel with no internal locking at all,
 * and one core is what makes disabling interrupts exclusion again.
 */
static intr_state_t
pool_enter(void)
{
    return md_intr_disable();
}

static void
pool_leave(intr_state_t s)
{
    md_intr_restore(s);
}

#endif /* SYS_TIER == 2 */

void *
pool_alloc(struct pool *p, sys_id_t *out_id)
{
    struct pool_slot *s;
    intr_state_t is;
    uint32_t index;
    void *obj;

    is = pool_enter();

    if (p->free_head == SYS_POOL_NIL) {
        pool_leave(is);
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

    if (out_id != NULL)
        *out_id = sys_id_make(index, s->gen);

    pool_leave(is);

    /*
     * Cleared outside the lock. The slot is this caller's from the
     * moment it left the list, and nobody else may touch it, so the
     * one part of this that is proportional to the object size is the
     * part nothing has to wait for.
     */
    memset(obj, 0, p->elem_size);

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
    intr_state_t is;
    uint32_t index;

    index = sys_id_index(id);
    if (index >= p->count)
        return false;

    is = pool_enter();

    s = &p->slots[index];
    if (!s->inuse || s->gen != sys_id_gen(id)) {
        pool_leave(is);
        return false;
    }

    s->gen = (s->gen + 1) & SYS_ID_GEN_MASK;
    if (s->gen == 0)
        s->gen = 1;

    s->inuse = false;
    s->next = p->free_head;
    p->free_head = index;
    p->used--;

    pool_leave(is);

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


/*
 * The live object in a slot, by position rather than by identifier, or
 * NULL if that slot holds nothing.
 *
 * For walking a pool, which nothing needed until M13 stage 6 wanted to
 * ask every live process what it was holding. Deliberately not a way to
 * name an object: an index is not an identifier, it carries no
 * generation, and a caller that kept one would be holding a name that
 * silently comes to mean the next occupant. Section 8's rule, which is
 * why pool_lookup takes the pair.
 */
void *
pool_at(const struct pool *p, uint32_t index)
{
    if (index >= p->count || !p->slots[index].inuse) {
        return NULL;
    }

    return slot_object(p, index);
}

uint32_t
pool_slots(const struct pool *p)
{
    return p->count;
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

void
pool_highwater_reset(struct pool *p)
{
    p->highwater = p->used;
}

void
pool_report_cores(void)
{
#if SYS_TIER == 2
    uint64_t total = 0;
    uint32_t i;

    /*
     * The high-water marks in the compared part of the report are one
     * number each for the machine, because a high-water mark is a
     * largest-ever over everything live at once and does not decompose
     * per core the way a count of events does. So they are kept under
     * a lock rather than added up, and this says the lock has work to
     * do: both cores take slots and give them back.
     */
    for (i = 0; i < cpu_count(); i++) {
        total += pool_ops[i];
    }

    cnprintf("pool slots: taken and given back %lu times,",
      (unsigned long)total);
    for (i = 0; i < cpu_count(); i++) {
        cnprintf("%s %lu on core %u", i == 0 ? "" : " and",
          (unsigned long)pool_ops[i], (unsigned int)i);
        if (pool_ops[i] == 0) {
            panic("core %u never touched a pool", (unsigned int)i);
        }
    }
    cnprintf("\n");
    cnputs("    one high-water mark for the machine, kept under a lock\n");
#endif
}
