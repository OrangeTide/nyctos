/*
 * SPDX-License-Identifier: 0BSD OR Apache-2.0
 *
 * PUBLIC DOMAIN (CC0-1.0)
 */

/*
 * The measurement facility of section 13.3, and the one thing it has to
 * establish before any figure it produces means anything: that there is
 * one clock, that both cores read the same one, and that it does not go
 * backwards.
 */

#include <sys/channel.h>
#include <sys/cpu.h>
#include <sys/intr.h>
#include <sys/measure.h>
#include <sys/proc.h>
#include <sys/systm.h>

#include <stdbool.h>
#include <sys/thread.h>

static void meas_record(struct meas *m, uint64_t took);

uint64_t
meas_now(void)
{
    return md_time_now();
}

uint64_t
meas_hz(void)
{
    return md_time_hz();
}

uint64_t
meas_enter(void)
{
    return md_time_now();
}

void
meas_leave_points(struct meas *m, uint64_t started, uint64_t points,
  bool counted)
{
    uint32_t self = cpu_self()->cp_index;
    uint64_t now = md_time_now();
    uint64_t took = now >= started ? now - started : 0;

    if (!counted) {
        m->ms_uncounted[self]++;
    } else {
        if (m->ms_count[self] == m->ms_uncounted[self] ||
          points < m->ms_plo[self]) {
            m->ms_plo[self] = points;
        }
        if (m->ms_count[self] == m->ms_uncounted[self] ||
          points > m->ms_phi[self]) {
            m->ms_phi[self] = points;
        }
    }

    if (m->ms_count[self] == 0 || took < m->ms_shortest[self]) {
        m->ms_shortest[self] = took;
    }

    /*
     * One reading of the clock for one visit. Calling the plain
     * version here would read it a second time, so the shortest and
     * the longest would be measured to different ends of the same
     * visit.
     */
    meas_record(m, took);
}

void
meas_report(const struct meas *m)
{
    uint64_t hz = md_time_hz();
    uint64_t n = meas_count(m);
    uint64_t lo = 0;
    uint64_t hi = 0;
    bool seen = false;
    uint32_t i;

    for (i = 0; i < SYS_MAX_CORES; i++) {
        if (m->ms_count[i] == 0) {
            continue;
        }
        if (!seen || m->ms_plo[i] < lo) {
            lo = m->ms_plo[i];
        }
        if (!seen || m->ms_phi[i] > hi) {
            hi = m->ms_phi[i];
        }
        seen = true;
    }

    if (n == 0) {
        cnprintf("    %s: none in this run\n", m->ms_name);
        return;
    }

    {
        uint64_t shortest = 0;
        uint64_t uncounted = 0;
        bool first = true;

        for (i = 0; i < SYS_MAX_CORES; i++) {
            uncounted += m->ms_uncounted[i];
            if (m->ms_count[i] == 0) {
                continue;
            }
            if (first || m->ms_shortest[i] < shortest) {
                shortest = m->ms_shortest[i];
                first = false;
            }
        }

        /*
         * The shortest is the one the kernel is answerable for. The
         * longest is whatever the server on the other end was doing,
         * and in this session that includes waiting for a person to
         * type, so it is a figure about the run rather than the path.
         */
        /*
         * In the clock's ticks and in microseconds. The ticks are what
         * was counted and the microseconds are what can be compared
         * with the other port, whose clock runs six times faster: the
         * same work gives wildly different tick counts and nearly the
         * same time, which is the only way to see that the two ports
         * agree about anything.
         */
        cnprintf("    %s: %lu of them, %lu ticks at best, "
          "%lu at worst\n", m->ms_name, (unsigned long)n,
          (unsigned long)shortest, (unsigned long)meas_longest(m));
        cnprintf("        best is about %lu microseconds\n",
          (unsigned long)(hz != 0 ? (shortest * 1000000UL) / hz : 0));

        if (uncounted == n) {
            cnprintf("        points: not countable, %lu crossed a core\n",
              (unsigned long)uncounted);
        } else if (lo == hi) {
            cnprintf("        points: %lu every time\n", (unsigned long)lo);
        } else {
            cnprintf("        points: %lu to %lu\n", (unsigned long)lo,
              (unsigned long)hi);
        }
    }
}

static void
meas_record(struct meas *m, uint64_t took)
{
    uint32_t self = cpu_self()->cp_index;

    m->ms_count[self]++;
    m->ms_total[self] += took;
    if (took > m->ms_longest[self]) {
        m->ms_longest[self] = took;
    }
}

void
meas_leave(struct meas *m, uint64_t started)
{
    uint64_t now = md_time_now();
    uint32_t self = cpu_self()->cp_index;
    uint64_t took;

    /*
     * A read that went backwards would make this enormous rather than
     * negative, and an enormous longest-so-far is a figure that never
     * moves again. The clock is checked once at boot instead, where
     * saying so is useful; here it is only kept from poisoning a
     * maximum.
     */
    took = now >= started ? now - started : 0;

    m->ms_count[self]++;
    m->ms_total[self] += took;
    if (took > m->ms_longest[self]) {
        m->ms_longest[self] = took;
    }
}

uint64_t
meas_count(const struct meas *m)
{
    uint64_t n = 0;
    uint32_t i;

    for (i = 0; i < SYS_MAX_CORES; i++) {
        n += m->ms_count[i];
    }

    return n;
}

uint64_t
meas_count_on(const struct meas *m, uint32_t core)
{
    if (core >= SYS_MAX_CORES) {
        return 0;
    }

    return m->ms_count[core];
}

uint64_t
meas_longest(const struct meas *m)
{
    uint64_t worst = 0;
    uint32_t i;

    /*
     * The largest of the per-core figures and not their sum. Two cores
     * each spending a microsecond in a region did not spend two
     * microseconds in it, and the question section 13.3 asks is how
     * long the worst visit was.
     */
    for (i = 0; i < SYS_MAX_CORES; i++) {
        if (m->ms_longest[i] > worst) {
            worst = m->ms_longest[i];
        }
    }

    return worst;
}

void
meas_report_clock(void)
{
    uint64_t hz = md_time_hz();

    /*
     * Above the compared line, and the wording is deliberate. This is
     * the resolution of a counter in an emulator, so every duration
     * derived from it is a statement about the machine this ran on and
     * not about the kernel.
     */
    cnprintf("clock: %lu ticks a second, which is %lu ns each\n",
      (unsigned long)hz,
      (unsigned long)(hz != 0 ? 1000000000UL / hz : 0));
    cnputs("    durations from it describe this host, not this kernel\n");
}

void
meas_check_clock(void)
{
    uint64_t first = md_time_now();
    uint64_t last = first;
    uint32_t i;

    /*
     * One clock, read many times, never going backwards. Both ports
     * take it from a counter the whole machine shares rather than from
     * anything per core, which is what makes a duration measured on one
     * core comparable with one measured on the other. A per-core
     * counter would pass every other check in this file and make every
     * figure that crosses a core meaningless.
     */
    /*
     * Read until it moves, with room enough that only a stopped clock
     * reaches the end. The bound is not a speed limit: this file says
     * never to assert a threshold on a duration, and an earlier
     * version of this loop did exactly that by insisting the clock
     * tick within ten thousand reads. What is being checked is that it
     * runs at all and never goes backwards, both of which are
     * properties of the counter rather than of the host.
     */
    for (i = 0; i < 100000000u; i++) {
        uint64_t now = md_time_now();

        if (now < last) {
            panic("clock: went backwards, %lu then %lu",
              (unsigned long)last, (unsigned long)now);
        }
        last = now;

        if (now != first) {
            return;
        }
    }

    panic("clock: it never moved, so it is stopped");
}

/*
 * Section 13.3's first term: the longest region with interrupts
 * disabled.
 *
 * Measured at the transition rather than at every call, because the
 * term the section defines is a contiguous region and this kernel nests
 * these freely. A call that finds interrupts already off is inside a
 * region somebody else began; the one that turns them off is the one
 * that starts the clock, and the restore that turns them back on is the
 * one that stops it.
 *
 * The site is the address of whoever asked, which is a name in the
 * sense that matters: a figure with nothing attached says a region was
 * long and gives nobody anywhere to look. Resolve it against the link
 * map of the build that produced it.
 *
 * Two clock reads per region is not free, and the figure includes them.
 * That is the price of the term being measured at all, and it is
 * recorded rather than netted out, because netting it out would mean
 * claiming a number this cannot produce.
 */
/*
 * Nothing is measured until the kernel has enabled interrupts for the
 * first time.
 *
 * Before that the machine is booting with them off, and the interval
 * from the first region that opens to the moment they are finally
 * enabled is one long disabled region that no path in the running
 * system ever produces. It was the largest figure in every run, by two
 * orders of magnitude over the mean, and it is an artefact of when the
 * kernel decides to start rather than of anything the kernel does.
 */
static bool intr_measuring;

void
intr_region_arm(void)
{
    intr_measuring = true;
}

void
intr_region_begin(uintptr_t site)
{
    struct cpu *c = cpu_self();

    if (!intr_measuring) {
        return;
    }

    /*
     * Nothing to charge it to yet. The boot path clears the register
     * this is found through, so a core that has not claimed its own
     * state reads NULL rather than whatever reset left, and the
     * measurement declines rather than writing through it.
     */
    if (c == NULL) {
        return;
    }

    c->cp_intr_site = site;
    c->cp_intr_open = true;
    c->cp_intr_start = md_time_now();
}

void
intr_region_end(uintptr_t site)
{
    struct cpu *c = cpu_self();

    if (!intr_measuring) {
        return;
    }
    uint64_t now;
    uint64_t took;

    if (c == NULL) {
        return;
    }

    if (!c->cp_intr_open) {
        /*
         * A region that was left open by something re-enabling
         * interrupts without going through the restore, which the
         * return to user mode and the idle path both do. Nothing is
         * charged for it: what is known is that it ended, not when.
         */
        return;
    }

    now = md_time_now();
    took = now >= c->cp_intr_start ? now - c->cp_intr_start : 0;

    c->cp_intr_open = false;
    c->cp_intr_regions++;
    c->cp_intr_total += took;

    if (took > c->cp_intr_worst) {
        c->cp_intr_worst = took;
        c->cp_intr_worst_site = c->cp_intr_site;
        c->cp_intr_worst_end = site;
        c->cp_intr_worst_at = c->cp_intr_start;
    }

    if (took > c->cp_win_worst) {
        c->cp_win_worst = took;
        c->cp_win_site = c->cp_intr_site;
    }
}

void
intr_region_report(void)
{
    uint32_t i;

    cnputs("interrupts off: the longest region, per section 13.3\n");

    for (i = 0; i < cpu_count(); i++) {
        const struct cpu *c = cpu_at(i);

        uint64_t hz = md_time_hz();

        uint64_t mean = c->cp_intr_regions != 0 ?
          c->cp_intr_total / c->cp_intr_regions : 0;

        /*
         * The mean beside the longest, because a bound drawn from one
         * sample is a bound drawn from one sample. Section 13.3 asks
         * for the worst case and the worst case is the right figure;
         * knowing whether it is typical or a single outlier is what
         * says whether it describes the kernel or an accident.
         */
        cnprintf("    core %u: %lu regions, longest %lu ticks "
          "(about %lu us), mean %lu ticks, begun at 0x%lx\n",
          (unsigned int)i,
          (unsigned long)c->cp_intr_regions,
          (unsigned long)c->cp_intr_worst,
          (unsigned long)(hz != 0 ? (c->cp_intr_worst * 1000000UL) / hz : 0),
          (unsigned long)mean,
          (unsigned long)c->cp_intr_worst_site);
        cnprintf("        ended at 0x%lx, and began %lu ticks into the "
          "run of %lu\n", (unsigned long)c->cp_intr_worst_end,
          (unsigned long)c->cp_intr_worst_at,
          (unsigned long)md_time_now());

        if (c->cp_intr_regions == 0) {
            panic("interrupts off: core %u never disabled them",
              (unsigned int)i);
        }
        if (c->cp_intr_worst_site == 0) {
            panic("interrupts off: core %u has a longest region "
              "with nobody to blame", (unsigned int)i);
        }
    }
}

uintptr_t
intr_region_worst_site(void)
{
    return cpu_self()->cp_intr_worst_site;
}

/*
 * A preemption point, per section 13.3: a place where the kernel could
 * be preempted, and therefore a boundary on how long it cannot be.
 *
 * The section names three of them, at batch entry boundaries, at
 * message copy chunk boundaries and at page table walk boundaries, and
 * until now the tree had none: the Tier 0 latency argument rested on a
 * placement that existed only in the section. These count rather than
 * yield, because what this milestone needs first is the number, and a
 * point that yields is a scheduling change rather than a measurement.
 */
void
preempt_point(void)
{
    struct cpu *c = cpu_self();
    struct thread *t;

    if (c != NULL) {
        c->cp_points++;
    }

    /*
     * And on the thread, because a path's length is a property of the
     * path. The core's count answers how many boundaries this core
     * went past, which is a different question and the one that was
     * being asked by mistake.
     */
    t = thread_current();
    if (t != NULL) {
        t->t_points++;
    }
}

uint64_t
preempt_points(void)
{
    struct cpu *c = cpu_self();

    return c != NULL ? c->cp_points : 0;
}

/*
 * Section 13.3's second term: from an interrupt being taken to the user
 * thread waiting on it running.
 *
 * Measured on the path the system already has rather than on one built
 * for the purpose. An interrupt handler that makes a thread runnable
 * stamps it; when that thread next returns to user mode the distance is
 * closed. The console driver is a loaded program waiting on the UART
 * interrupt's timeline, so the path is walked for real in every run.
 */
void
irq_dispatch_stamp(struct thread *t)
{
    struct cpu *c = cpu_self();

    if (c == NULL || !c->cp_in_irq) {
        return;
    }

    /*
     * Section 13.3's second term is the time to dispatch an interrupt
     * to a user thread, and the kernel's own threads are not that. One
     * of them reaching the same closing call through a system call it
     * made itself put a batch of sixteen preemption points into a
     * figure that is supposed to be the length of a dispatch.
     */
    if (t->t_proc == proc_kernel()) {
        return;
    }

    t->t_irq_stamp = md_time_now();
    t->t_irq_points = t->t_points;
}

void
irq_dispatch_arrived(struct thread *t)
{
    struct cpu *c = cpu_self();
    uint64_t now;
    uint64_t took;

    if (c == NULL || t == NULL || t->t_irq_stamp == 0) {
        return;
    }

    now = md_time_now();
    took = now >= t->t_irq_stamp ? now - t->t_irq_stamp : 0;

    c->cp_disp_count++;
    if (took > c->cp_disp_longest) {
        c->cp_disp_longest = took;
        c->cp_disp_points = t->t_points - t->t_irq_points;
    }

    t->t_irq_stamp = 0;
    t->t_irq_points = 0;
}

void
irq_dispatch_report(void)
{
    uint64_t hz = md_time_hz();
    uint32_t i;
    uint64_t seen = 0;

    cnputs("interrupt to a program: section 13.3's second term\n");

    for (i = 0; i < cpu_count(); i++) {
        const struct cpu *c = cpu_at(i);

        seen += c->cp_disp_count;
        if (c->cp_disp_count == 0) {
            continue;
        }
        cnprintf("    core %u: %lu dispatched, longest %lu ticks "
          "(about %lu us), %lu points passed\n", (unsigned int)i,
          (unsigned long)c->cp_disp_count,
          (unsigned long)c->cp_disp_longest,
          (unsigned long)(hz != 0 ?
            (c->cp_disp_longest * 1000000UL) / hz : 0),
          (unsigned long)c->cp_disp_points);
    }

    /*
     * A run with none is a fact about the run and not about the
     * kernel, so it is said rather than panicked over. The paced
     * session dispatches sixteen of these because the console driver
     * waits for a person; the burst session dispatches none, because
     * the whole input is already there when the driver looks and it
     * never blocks. Insisting on one would be insisting on how the
     * input arrived.
     *
     * What is asserted is the thing that is about the kernel: a
     * dispatch that happened passed no preemption point, so the path
     * from an interrupt to the program waiting on it is straight and
     * cannot be broken into anywhere.
     */
    for (i = 0; i < cpu_count(); i++) {
        const struct cpu *c = cpu_at(i);

        /*
         * How often the handler let something else run while it was
         * still the handler. Not a defect and not work: it is the
         * reason the flag saying so belongs to what is running rather
         * than to the core.
         */
        cnprintf("    core %u handed the core away %lu times "
          "mid-handler\n", (unsigned int)i,
          (unsigned long)c->cp_irq_switches);
    }

    if (seen == 0) {
        cnputs("    none in this run, which is a fact about the input\n");
        return;
    }

    for (i = 0; i < cpu_count(); i++) {
        const struct cpu *c = cpu_at(i);

        if (c->cp_disp_count != 0 && c->cp_disp_points != 0) {
            panic("interrupt to a program: %lu points on the way",
              (unsigned long)c->cp_disp_points);
        }
    }
}


/*
 * The two figures the design's own arguments rest on, taken on the
 * paths the system already has rather than on a benchmark written to
 * come out well.
 *
 * The round trip is what a client spends blocked waiting for its reply.
 * The donation is from a client raising a server's priority at the
 * rendezvous to that server running: section 12.3's mechanism, timed
 * rather than argued about.
 *
 * The donation is kept in two, because M8 measured a boundary between
 * cores at four inter-processor interrupts and this is the same
 * crossing seen from the other side. A donation that stays on one core
 * needs nobody told; one that crosses needs a poke delivered and a
 * core woken before the server can run at all.
 */
static struct meas meas_roundtrip = { .ms_name = "round trip" };
static struct meas meas_donate_near = { .ms_name = "donation, same core" };
static struct meas meas_donate_far = { .ms_name = "donation, across cores" };

uint64_t
ipc_round_begin(void)
{
    return md_time_now();
}

void
ipc_round_end(uint64_t started, uint64_t points)
{
    if (started == 0) {
        return;
    }

    meas_leave_points(&meas_roundtrip, started, preempt_points() - points,
      true);
}

void
ipc_donate_stamp(struct thread *to, bool across)
{
    /*
     * A thread raising its own priority is not a donation anybody
     * waits for. A server picking up a queued sender does exactly
     * that, and it is already running: stamping it means the arrival
     * is recorded at the next message this thread blocks for, which
     * put the two seconds between two typed lines into a figure about
     * the kernel.
     */
    if (to == thread_current()) {
        return;
    }

    to->t_don_stamp = md_time_now();
    to->t_don_points = preempt_points();
    to->t_don_core = cpu_self()->cp_index;
    to->t_don_across = across;
}

void
ipc_donate_arrived(struct thread *t)
{
    if (t->t_don_stamp == 0) {
        return;
    }

    /*
     * The points are only a number if both ends were counted on the
     * same core. A donation that crossed was stamped on one core and
     * arrives on another, and those counters have nothing to do with
     * each other.
     */
    meas_leave_points(t->t_don_across ? &meas_donate_far : &meas_donate_near,
      t->t_don_stamp, preempt_points() - t->t_don_points,
      t->t_don_core == cpu_self()->cp_index);

    t->t_don_stamp = 0;
}

void
ipc_report(void)
{
    {
        uint64_t deepest;
        uint64_t raised = channel_chain_raised(&deepest);

        cnprintf("chains: %lu threads raised by a walk, deepest %lu of "
          "%u\n", (unsigned long)raised, (unsigned long)deepest,
          (unsigned int)SYS_PRIO_CHAIN_MAX);
    }

    cnputs("ipc: what the round trip and the donation cost\n");
    meas_report(&meas_roundtrip);
    meas_report(&meas_donate_near);
    meas_report(&meas_donate_far);
}

/*
 * The longest region since this was last cleared, for asking what one
 * particular thing did rather than what the whole run did.
 *
 * The running maximum answers section 13.3's question about the system.
 * It cannot answer whether a batch of sixteen entries holds interrupts
 * off sixteen times as long as a batch of one, because that is a
 * question about two windows and the maximum is one number over all of
 * them.
 */
void
intr_region_reset(void)
{
    struct cpu *c = cpu_self();

    if (c == NULL) {
        return;
    }

    c->cp_win_worst = 0;
    c->cp_win_site = 0;
}

uint64_t
intr_region_worst(uintptr_t *site)
{
    struct cpu *c = cpu_self();

    if (c == NULL) {
        *site = 0;
        return 0;
    }

    *site = c->cp_win_site;

    return c->cp_win_worst;
}

uint64_t
submit_crossings(uint64_t *checks)
{
    struct cpu *c = cpu_self();

    *checks = c->cp_checks;

    return c->cp_syscalls;
}
