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
    for (i = 0; i < 10000; i++) {
        uint64_t now = md_time_now();

        if (now < last) {
            panic("clock: went backwards, %lu then %lu",
              (unsigned long)last, (unsigned long)now);
        }
        last = now;
    }

    if (last == first) {
        panic("clock: %lu reads and it never moved", (unsigned long)i);
    }
}
