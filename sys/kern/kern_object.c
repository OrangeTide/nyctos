/*
 * SPDX-License-Identifier: 0BSD OR Apache-2.0
 * Copyright (C) 2026 Jon Mayo <jon@rm-f.net>
 */

/*
 * The kernel's object pools: one per type, all of them in .bss, all of them
 * sized by sys/sys/config.h. This file is where the sizes in that header
 * become memory.
 */

#include <sys/prepared.h>
#include <sys/channel.h>
#include <sys/config.h>
#include <sys/object.h>
#include <sys/page.h>
#include <sys/pool.h>
#include <sys/systm.h>
#include <sys/vm.h>

POOL_STORAGE(procs, struct proc, SYS_MAX_PROCESSES);
POOL_STORAGE(threads, struct thread, SYS_MAX_THREADS);
POOL_STORAGE(channels, struct channel, SYS_MAX_CHANNELS);
POOL_STORAGE(connections, struct connection, SYS_MAX_CONNECTIONS);
POOL_STORAGE(grants, struct grant, SYS_MAX_GRANTS);
POOL_STORAGE(timelines, struct timeline, SYS_MAX_TIMELINES);
POOL_STORAGE(timers, struct timer, SYS_MAX_TIMERS);
POOL_STORAGE(programs, struct program, SYS_MAX_PROGRAMS);
POOL_STORAGE(prepared, struct prepared, SYS_MAX_PREPARED);

/*
 * Every pool has to fit the index field of an identifier, and pool_init()
 * checks that at boot. It is a build-time fact, though, so it is also
 * checked here: the ceiling is a thousand slots on a 32-bit part rather
 * than a million on a 64-bit one, which makes it a bound a configuration
 * could actually cross.
 */
#define POOL_FITS(n)  _Static_assert((n) > 0 && (n) < SYS_POOL_MAX_SLOTS, \
  #n " does not fit the index field of an identifier; see sys/pool.h")

POOL_FITS(SYS_MAX_PROCESSES);
POOL_FITS(SYS_MAX_THREADS);
POOL_FITS(SYS_MAX_CHANNELS);
POOL_FITS(SYS_MAX_CONNECTIONS);
POOL_FITS(SYS_MAX_GRANTS);
POOL_FITS(SYS_MAX_TIMELINES);
POOL_FITS(SYS_MAX_TIMERS);
POOL_FITS(SYS_MAX_PROGRAMS);
POOL_FITS(SYS_MAX_HANDLES_PER_PROC);

static struct pool proc_p;
static struct pool thread_p;
static struct pool channel_p;
static struct pool connection_p;
static struct pool grant_p;
static struct pool timeline_p;
static struct pool timer_p;
static struct pool program_p;
static struct pool prepared_p;

struct pool *proc_pool(void) { return &proc_p; }
struct pool *thread_pool(void) { return &thread_p; }
struct pool *channel_pool(void) { return &channel_p; }
struct pool *connection_pool(void) { return &connection_p; }
struct pool *grant_pool(void) { return &grant_p; }
struct pool *timeline_pool(void) { return &timeline_p; }
struct pool *timer_pool(void) { return &timer_p; }
struct pool *program_pool(void) { return &program_p; }
struct pool *prepared_pool(void) { return &prepared_p; }

static struct pool *const all_pools[] = {
    &proc_p,
    &thread_p,
    &channel_p,
    &connection_p,
    &grant_p,
    &timeline_p,
    &timer_p,
    &program_p,
    &prepared_p,
};

#define NPOOLS (sizeof(all_pools) / sizeof(all_pools[0]))

void
object_init(void)
{
    POOL_INIT(&proc_p, procs, "proc", SYS_MAX_PROCESSES);
    POOL_INIT(&thread_p, threads, "thread", SYS_MAX_THREADS);
    POOL_INIT(&channel_p, channels, "channel", SYS_MAX_CHANNELS);
    POOL_INIT(&connection_p, connections, "connection", SYS_MAX_CONNECTIONS);
    POOL_INIT(&grant_p, grants, "grant", SYS_MAX_GRANTS);
    POOL_INIT(&timeline_p, timelines, "timeline", SYS_MAX_TIMELINES);
    POOL_INIT(&timer_p, timers, "timer", SYS_MAX_TIMERS);
    POOL_INIT(&program_p, programs, "program", SYS_MAX_PROGRAMS);
    POOL_INIT(&prepared_p, prepared, "prepared", SYS_MAX_PREPARED);
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

/*
 * What the run actually used, as opposed to what it was configured for.
 *
 * Milestone 3 exists partly to answer whether one thread per event source
 * is affordable, and section 8's answer to every question like it is a
 * number rather than an impression. The high-water mark is the number: it
 * says how many of each object were live at once, which is what a build
 * has to size for, and nothing here is reaped before M7 so it is also the
 * total ever created.
 */
void
object_report_usage(void)
{
    const struct pool *threads = thread_pool();
    const struct pool *procs = proc_pool();

    cnprintf("used at most: %u of %u threads, %u of %u processes\n",
      threads->highwater, threads->count, procs->highwater, procs->count);
    cnprintf("  kernel stacks under those threads: %u bytes\n",
      (unsigned int)(threads->highwater * SYS_KSTACK_SIZE));

    /*
     * And the one pool nothing gives back, per M20 stage one.
     *
     * Reported the way the others are and with one number none of them
     * needs: how many of the slots in use nothing in the system can
     * reach. Every other pool here ends a boot below its own
     * high-water mark, because a process, a thread, a connection, a
     * prepared program and a timer all go back. A channel does not, so
     * these two numbers are equal, and they will stay equal until
     * stage three.
     *
     * In the compared part of the report because all three are counts.
     * What a boot spends on channels is what its checks ask for and is
     * the same in every run at a tier; how long it took to spend them
     * is not here, because a duration is the host's.
     */
    {
        const struct pool *chans = channel_pool();

        uint32_t lost = channel_unreachable();

        cnprintf("  channels: %u in use, %u at once, of %u, and %u "
          "unreachable\n", (unsigned int)chans->used,
          (unsigned int)chans->highwater, (unsigned int)chans->count,
          (unsigned int)lost);

        /*
         * Asserted as well as compared, because the compared file
         * covers the paced run and the burst run is only read for a
         * panic. A number this milestone turns on should fail in both.
         *
         * Both of these are assertions that a defect is present, which
         * is unusual and is deliberate. A pool where nothing is
         * reachable but everything is still held is the subject of
         * this milestone, and if the first of them ever passes on its
         * own then the milestone has no subject and should be
         * abandoned rather than built. Stage three makes the second
         * one false on purpose and rewrites it.
         */
        if (lost == 0) {
            panic("channels: nothing is unreachable, so there is "
              "nothing here to give back");
        }
        if (chans->used != chans->highwater) {
            panic("channels: %u in use against %u at once, so "
              "something already gives one back",
              (unsigned int)chans->used,
              (unsigned int)chans->highwater);
        }
    }

    /*
     * And what remapping buffers between address spaces cost, per
     * section 18. In the compared part of the report because a number
     * nobody would notice changing is a number that stops meaning
     * anything.
     *
     * Remapping, and not moving. Zero here does not mean no buffers
     * were exchanged: the same messages carrying the same buffers
     * cross at both tiers, which the rendezvous counts in the boot
     * report show. It means none of it had to be remapped, because
     * there is one address space and a buffer that arrives is already
     * where it needs to be. The whole of this line is what the other
     * tier pays for keeping programs apart.
     */
    {
        struct vm_cost c;

        vm_cost_read(&c);
        cnprintf("  buffer remapping: %u pages in %u halves, "
          "%u entries, %u tables, %u invalidations, %u flushes\n",
          (unsigned int)c.vc_pages, (unsigned int)c.vc_halves,
          (unsigned int)c.vc_leaf, (unsigned int)c.vc_table,
          (unsigned int)c.vc_inval, (unsigned int)c.vc_flush);
    }
}
