/* cpu_md.c : RISC-V per-hart pointer and hart startup.
 *
 * SPDX-License-Identifier: 0BSD OR Apache-2.0
 * Copyright (C) 2026 Jon Mayo <jon@rm-f.net>
 *
 * Holds the current struct cpu pointer in tp. With no firmware present,
 * md_cpu_start stores the entry point in hart_entry and sets hart_release
 * for the target hart, which is spinning in boot.S. md_cpu_available
 * counts the harts that have parked, waiting up to one million polls for
 * each.
 *
 */

#include <sys/backtrace.h>
#include <sys/cpu.h>
#include <sys/systm.h>

/* Written by boot.S when a hart parks, read here to know it is there. */
extern volatile uint64_t hart_present[SYS_MAX_CORES];
extern volatile uint64_t hart_release[SYS_MAX_CORES];
extern volatile uint64_t hart_entry;

void
md_cpu_set_self(struct cpu *c)
{
    __asm__ volatile ("mv tp, %0" :: "r" (c));
}

struct cpu *
md_cpu_get_self(void)
{
    struct cpu *c;

    __asm__ volatile ("mv %0, tp" : "=r" (c));

    return c;
}

uint32_t
md_cpu_available(void)
{
    uint32_t n = 1;

    /*
     * However many parked. A hart that never reached the parking loop
     * is one this kernel cannot use and must not count, and counting
     * from the board's own description would be counting harts that
     * exist rather than harts that answered.
     *
     * Waited for rather than sampled. Every hart leaves reset at the
     * same moment and this runs on the one that went on into C, so a
     * hart that has not stored its word yet has not gone missing, it
     * is a few instructions behind. Reading once would make the count
     * depend on how long the boot hart spent zeroing memory, which is
     * a number that has nothing to do with how many cores there are.
     */
    while (n < SYS_MAX_CORES) {
        uint32_t spin;

        for (spin = 0; spin < 1000000 && hart_present[n] == 0; spin++) {
            md_cpu_relax();
        }
        if (hart_present[n] == 0) {
            break;
        }
        n++;
    }

    return n;
}

sys_err_t
md_cpu_start(uint32_t index)
{
    if (index >= SYS_MAX_CORES || hart_present[index] == 0) {
        return SYS_ERR_NO_ENT;
    }

    hart_entry = (uintptr_t)cpu_secondary_entry;
    __asm__ volatile ("fence w, w" ::: "memory");
    hart_release[index] = 1;

    return SYS_OK;
}

void
md_cpu_fence(void)
{
    __asm__ volatile ("fence rw, rw" ::: "memory");
}

void
md_cpu_relax(void)
{
    /*
     * Zihintpause if the part has it, and a nop if it does not: the
     * encoding is a fence with an operand no fence uses, so a part
     * without the extension executes it as one it can ignore.
     */
    __asm__ volatile (".insn i 0x0F, 0, x0, x0, 0x010" ::: "memory");
}

void
md_cpu_wait(void)
{
    __asm__ volatile ("wfi" ::: "memory");
}

uint32_t
md_cpu_arriving(void)
{
    uint64_t id;

    /*
     * The hart identifier, which supervisor mode cannot read: mhartid
     * is machine mode's. boot.S left it in tp on the way down, which is
     * the register this core is about to keep its own state in, so it
     * is read once here and overwritten immediately.
     */
    __asm__ volatile ("mv %0, tp" : "=r" (id));

    return (uint32_t)id;
}

/*
 * One frame of the RISC-V call stack. With frame pointers, a function's
 * prologue stores its return address and the caller's frame pointer just
 * below the frame pointer, s0: the return address at offset minus one and
 * the caller's frame pointer at offset minus two.
 */
bool
md_frame_step(uintptr_t fp, uintptr_t *ra, uintptr_t *caller_fp)
{
    const uintptr_t *f = (const uintptr_t *)fp;

    *ra = f[-1];
    *caller_fp = f[-2];

    return true;
}
