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
};

/* The monotonic source, and how many of its ticks are a second. */
uint64_t meas_now(void);
uint64_t meas_hz(void);

/*
 * Open and close a region. The value returned by the first is handed
 * back to the second, so nothing is kept anywhere that two cores could
 * be inside at once.
 */
uint64_t meas_enter(void);
void meas_leave(struct meas *m, uint64_t started);

/* How many times this region has been entered, on every core. */
uint64_t meas_count(const struct meas *m);

/*
 * And on one of them. A total alone cannot tell a per-core counter from
 * a single shared one, because one core doing all the work produces the
 * same total either way.
 */
uint64_t meas_count_on(const struct meas *m, uint32_t core);

/* The longest single visit, on any core, in the ticks of meas_hz(). */
uint64_t meas_longest(const struct meas *m);

/*
 * What the clock is, and what it is a clock of. Printed above the
 * compared line because its resolution is a fact about the board.
 */
void meas_report_clock(void);

/* That it is one clock and it does not go backwards. Panics if it does. */
void meas_check_clock(void);

/*
 * Section 13.3's first term. The ports call these at the transition
 * into and out of interrupts being disabled, which is where a region
 * begins and ends however deeply the calls nest inside it.
 */
void intr_region_begin(uintptr_t site);
void intr_region_end(void);
void intr_region_report(void);
uintptr_t intr_region_worst_site(void);

#endif /* _SYS_MEASURE_H_ */
