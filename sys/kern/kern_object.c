/*
 * SPDX-License-Identifier: 0BSD OR Apache-2.0
 * Copyright (C) 2026 Jon Mayo <jon@rm-f.net>
 */

/*
 * The kernel's object pools: one per type, all of them in .bss, all of them
 * sized by sys/sys/config.h. This file is where the sizes in that header
 * become memory.
 */

#include <sys/config.h>
#include <sys/object.h>
#include <sys/pool.h>
#include <sys/systm.h>

POOL_STORAGE(procs, struct proc, SYS_MAX_PROCESSES);
POOL_STORAGE(threads, struct thread, SYS_MAX_THREADS);
POOL_STORAGE(channels, struct channel, SYS_MAX_CHANNELS);
POOL_STORAGE(grants, struct grant, SYS_MAX_GRANTS);
POOL_STORAGE(timelines, struct timeline, SYS_MAX_TIMELINES);
POOL_STORAGE(timers, struct timer, SYS_MAX_TIMERS);
POOL_STORAGE(programs, struct program, SYS_MAX_PROGRAMS);

static struct pool proc_p;
static struct pool thread_p;
static struct pool channel_p;
static struct pool grant_p;
static struct pool timeline_p;
static struct pool timer_p;
static struct pool program_p;

struct pool *proc_pool(void) { return &proc_p; }
struct pool *thread_pool(void) { return &thread_p; }
struct pool *channel_pool(void) { return &channel_p; }
struct pool *grant_pool(void) { return &grant_p; }
struct pool *timeline_pool(void) { return &timeline_p; }
struct pool *timer_pool(void) { return &timer_p; }
struct pool *program_pool(void) { return &program_p; }

static struct pool *const all_pools[] = {
    &proc_p,
    &thread_p,
    &channel_p,
    &grant_p,
    &timeline_p,
    &timer_p,
    &program_p,
};

#define NPOOLS (sizeof(all_pools) / sizeof(all_pools[0]))

void
object_init(void)
{
    POOL_INIT(&proc_p, procs, "proc", SYS_MAX_PROCESSES);
    POOL_INIT(&thread_p, threads, "thread", SYS_MAX_THREADS);
    POOL_INIT(&channel_p, channels, "channel", SYS_MAX_CHANNELS);
    POOL_INIT(&grant_p, grants, "grant", SYS_MAX_GRANTS);
    POOL_INIT(&timeline_p, timelines, "timeline", SYS_MAX_TIMELINES);
    POOL_INIT(&timer_p, timers, "timer", SYS_MAX_TIMERS);
    POOL_INIT(&program_p, programs, "program", SYS_MAX_PROGRAMS);
}

/*
 * Report the configuration, and the memory it costs. The total is the point
 * of the whole arrangement: it is a property of config.h and cannot be moved
 * by anything an application does.
 */
void
object_report(void)
{
    const struct pool *p;
    size_t total = 0;
    size_t bytes;
    size_t i;

    cnputs("pools:\n");
    for (i = 0; i < NPOOLS; i++) {
        p = all_pools[i];
        bytes = (size_t)p->count * (p->elem_size + sizeof(struct pool_slot));
        total += bytes;
        cnprintf("  %s: %u slots, %u bytes each, %zu bytes total\n",
          p->name, p->count, p->elem_size, bytes);
    }
    cnprintf("  kernel object memory: %zu bytes\n", total);
}

/*
 * Check at boot that the pool machinery does what sections 7.1 and 8 say it
 * does. This is a handful of allocations against the smallest pool, and it
 * runs once. It exists because every later stage builds on these invariants,
 * so the cheapest place to discover they are broken is here, before anything
 * depends on them.
 */
void
object_check(void)
{
    struct pool *p = program_pool();
    sys_id_t first, again, id;
    void *obj;
    uint32_t n;

    obj = pool_alloc(p, &first);
    if (obj == NULL || first == SYS_ID_INVALID)
        panic("pool: allocation from an empty pool failed");
    if (pool_lookup(p, first) != obj)
        panic("pool: live identifier did not resolve");
    if (pool_id_of(p, obj) != first)
        panic("pool: object did not name its own identifier");

    if (!pool_free(p, first))
        panic("pool: free of a live identifier failed");
    if (pool_lookup(p, first) != NULL)
        panic("pool: freed identifier still resolves");
    if (pool_free(p, first))
        panic("pool: double free was accepted");

    obj = pool_alloc(p, &again);
    if (obj == NULL)
        panic("pool: reallocation failed");
    if (again == first)
        panic("pool: reused slot handed out the same identifier");
    if (sys_id_index(again) != sys_id_index(first))
        panic("pool: expected the freed slot back");
    if (pool_lookup(p, first) != NULL)
        panic("pool: stale identifier resolved after reuse");
    if (!pool_free(p, again))
        panic("pool: free after reuse failed");

    /* Exhaustion is reported, not fatal, and the pool comes back intact. */
    for (n = 0; pool_alloc(p, &id) != NULL; n++)
        continue;
    if (n != p->count)
        panic("pool: exhausted after %u of %u slots", n, p->count);
    for (n = 0; n < p->count; n++) {
        if (!pool_free(p, pool_id_at(p, n)))
            panic("pool: could not drain slot %u", n);
    }
    if (p->used != 0)
        panic("pool: %u slots leaked", p->used);

    cnputs("pools: invariants ok\n");
}
