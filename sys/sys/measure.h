/*
 * SPDX-License-Identifier: 0BSD OR Apache-2.0
 *
 * PUBLIC DOMAIN (CC0-1.0)
 */

/*
 * Measurement, and the difference between two kinds of number.
 *
 * This kernel has never run on hardware. A duration taken under an
 * emulator is a duration of the emulator and of whatever else the host
 * was doing, and M8 established that rather than assuming it: a check
 * that asserted an interrupt arrived inside one particular run failed
 * about one run in three with the machine busy and never with it quiet.
 *
 * So every number here is one of two kinds and is treated differently.
 *
 * A **count** is a property of this kernel. How many times a region was
 * entered, how many switches a path cost, how many entries a page table
 * walk wrote. It is the same on every host, it belongs in the part of
 * the report the check compares exactly, and it is what section 13.3's
 * first term actually asks for when it says the longest region is
 * measurable on a given port.
 *
 * A **duration** is a property of the machine this ran on. It is
 * reported above the compared line, next to the resolution of the clock
 * that produced it, and nothing is ever asserted about its value. A
 * threshold on one of these is a check that passes on a fast host and
 * fails on a slow one, which is the failure that gets a check deleted.
 *
 * The unit a duration would most usefully be in is retired instructions,
 * which does not move when the host does. Neither port can supply one
 * here: the RISC-V counter is architecturally an instruction count and
 * this emulator answers it with something time-shaped, giving 130000 for
 * a thousand-iteration loop and the same figure again for a loop of
 * twice the length. That is recorded because it is the reason this file
 * measures durations in a clock nobody should trust and counts in
 * integers everybody can.
 */

#ifndef _SYS_MEASURE_H_
#define _SYS_MEASURE_H_

#include <sys/config.h>

#include <stdbool.h>
#include <stdint.h>

/*
 * A region worth knowing the size of, measured on the core that ran it.
 *
 * Per core because a region runs on a core: two cores entering the same
 * region are two separate longest-so-far figures, and adding them would
 * produce a number describing nothing.
 */
struct meas {
    const char *ms_name;
    uint64_t    ms_count[SYS_MAX_CORES];
    uint64_t    ms_longest[SYS_MAX_CORES];
    uint64_t    ms_total[SYS_MAX_CORES];

    /*
     * The fewest and the most preemption points any single visit
     * passed. Kept as a range rather than a total because the question
     * this milestone asks of a count is whether it is the same every
     * time: a range that has not widened is a number about this
     * kernel, and one that has is a number about the run.
     */
    uint64_t    ms_plo[SYS_MAX_CORES];
    uint64_t    ms_phi[SYS_MAX_CORES];

    /*
     * Visits whose points could not be counted, because they began on
     * one core and ended on another. A point counter is per core, and
     * subtracting one core's from another's produces a number that
     * means nothing: the first version of this printed one, and it had
     * wrapped past zero.
     */
    uint64_t    ms_uncounted[SYS_MAX_CORES];

    /* The shortest visit, which is the one the kernel is responsible
     * for when the longest is dominated by what the server did. */
    uint64_t    ms_shortest[SYS_MAX_CORES];
};

/* The monotonic source, and how many of its ticks are a second. */
uint64_t meas_now(void);

/*
 * Open and close a region. The value returned by the first is handed
 * back to the second, so nothing is kept anywhere that two cores could
 * be inside at once.
 */
uint64_t meas_enter(void);
void meas_leave(struct meas *m, uint64_t started);

/*
 * The same, with the preemption points the visit passed. The duration
 * is a fact about the host and the points are a fact about the kernel,
 * so they are reported apart and only the second is compared.
 */
void meas_leave_points(struct meas *m, uint64_t started, uint64_t points,
  bool counted);

/* One line: how many, how long at worst, and how many points. */
void meas_report(const struct meas *m);

/* How many times this region has been entered, on every core. */
uint64_t meas_count(const struct meas *m);

/*
 * And on one of them. A total alone cannot tell a per-core counter from
 * a single shared one, because one core doing all the work produces the
 * same total either way.
 */
uint64_t meas_count_on(const struct meas *m, uint32_t core);

/* The longest single visit, on any core, in the clock's ticks. */
uint64_t meas_longest(const struct meas *m);

/*
 * What the clock is, and what it is a clock of. Printed above the
 * compared line because its resolution is a fact about the board.
 */
void meas_report_clock(void);

struct thread;

/*
 * A preemption point, per section 13.3. Counts rather than yields: what
 * this milestone needs first is the number.
 */
void preempt_point(void);

/*
 * Mark a moment the kernel could refresh a stored uptime, and read back
 * how many there were and the longest gap between two. See
 * kern_measure.c and M16 stage one.
 */
void clock_mark(void);
uint64_t clock_mark_count(uint64_t *worst, const char **who);

struct thread;

/*
 * Leave this thread's dispatch out of section 13.3's second term. For a
 * thread woken by its own deadline with work to do on the way out; see
 * kern_measure.c.
 */
void irq_dispatch_forget(struct thread *t);
uint64_t preempt_points(void);

/*
 * Section 13.3's second term. The first is called where an interrupt
 * handler makes a thread runnable, the second where a user thread
 * returns to its program.
 */
void irq_dispatch_stamp(struct thread *t);
void irq_dispatch_arrived(struct thread *t);
void irq_dispatch_report(void);

/*
 * The round trip a client waits through, and the donation from being
 * raised to running. Both on the paths the system already has.
 */
uint64_t ipc_round_begin(void);
void ipc_round_end(uint64_t started, uint64_t points, uint32_t core);
void ipc_donate_stamp(struct thread *to, bool across);
void ipc_donate_arrived(struct thread *t);
void ipc_report(void);

/* That it is one clock and it does not go backwards. Panics if it does. */
void meas_check_clock(void);

/*
 * And that the other core reads the same counter, which every figure
 * that crosses a core depends on. The first is called on the boot
 * core, the second on each core that comes up after it.
 */
void meas_check_clock_across(void);
void meas_clock_secondary(void);

/*
 * Section 13.3's first term. The ports call these at the transition
 * into and out of interrupts being disabled, which is where a region
 * begins and ends however deeply the calls nest inside it.
 */
void intr_region_arm(void);
void intr_region_begin(uintptr_t site);
void intr_region_end(uintptr_t site);

/*
 * The region just opened was opened to print, so it is the console's
 * and not the kernel's. Both are reported.
 */
void intr_region_console(void);

/*
 * The switch itself: stamped by the thread leaving the core and read
 * by whichever thread it comes back in.
 */
void switch_leaving(void);
void switch_arrived(void);

void intr_region_report(void);

/*
 * Clear the running maximum and read it back, for asking what one
 * particular thing cost rather than what the whole run did.
 */
/*
 * Crossings of the trap edge on this core, and checks made on the
 * submission path. Both are counts and both are the same on every host.
 */
uint64_t submit_crossings(uint64_t *checks);

void intr_region_reset(void);
uint64_t intr_region_worst(uintptr_t *site);

/*
 * How many interrupts-off regions the window held. The countable half
 * of the same question the worst-case duration asks.
 */
uint64_t intr_region_count(void);

#endif /* _SYS_MEASURE_H_ */
