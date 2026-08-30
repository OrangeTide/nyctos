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

#include <sys/cpu.h>
#include <sys/intr.h>
#include <sys/measure.h>
#include <sys/systm.h>

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
void
intr_region_begin(uintptr_t site)
{
    struct cpu *c = cpu_self();

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
intr_region_end(void)
{
    struct cpu *c = cpu_self();
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

    if (took > c->cp_intr_worst) {
        c->cp_intr_worst = took;
        c->cp_intr_worst_site = c->cp_intr_site;
    }
}

void
intr_region_report(void)
{
    uint32_t i;

    cnputs("interrupts off: the longest region, per section 13.3\n");

    for (i = 0; i < cpu_count(); i++) {
        const struct cpu *c = cpu_at(i);

        cnprintf("    core %u: %lu regions, longest %lu ticks, "
          "begun at 0x%lx\n", (unsigned int)i,
          (unsigned long)c->cp_intr_regions,
          (unsigned long)c->cp_intr_worst,
          (unsigned long)c->cp_intr_worst_site);

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
