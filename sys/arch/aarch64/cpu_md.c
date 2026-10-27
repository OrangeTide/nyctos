/* cpu_md.c : AArch64 per-core pointer and secondary core start.
 *
 * SPDX-License-Identifier: 0BSD OR Apache-2.0
 * Copyright (C) 2026 Jon Mayo <jon@rm-f.net>
 *
 * Holds the current struct cpu pointer in TPIDR_EL1 and reads the core
 * number from MPIDR_EL1. Starts a secondary core by calling PSCI CPU_ON
 * through hvc #0, with secondary_entry from boot.S as the entry point.
 * The core count is SYS_MAX_CORES, since this port does not enumerate
 * cores from a device tree.
 *
 */

#include <sys/cpu.h>
#include <sys/systm.h>

#define PSCI_CPU_ON     0xc4000003u     /* the 64-bit calling convention */

/* Where a started core arrives. See boot.S. */
extern void secondary_entry(void);

static long
psci_call(uint64_t function, uint64_t a1, uint64_t a2, uint64_t a3)
{
    register uint64_t x0 __asm__("x0") = function;
    register uint64_t x1 __asm__("x1") = a1;
    register uint64_t x2 __asm__("x2") = a2;
    register uint64_t x3 __asm__("x3") = a3;

    __asm__ volatile ("hvc #0"
      : "+r" (x0) : "r" (x1), "r" (x2), "r" (x3) : "memory");

    return (long)x0;
}

void
md_cpu_set_self(struct cpu *c)
{
    __asm__ volatile ("msr tpidr_el1, %0" :: "r" (c));
}

struct cpu *
md_cpu_get_self(void)
{
    struct cpu *c;

    __asm__ volatile ("mrs %0, tpidr_el1" : "=r" (c));

    return c;
}

uint32_t
md_cpu_available(void)
{
    /*
     * What the build asks for. This port cannot enumerate without a
     * device tree, and a core that is not there refuses to start, which
     * is the answer arriving one call later rather than not at all.
     */
    return SYS_MAX_CORES;
}

sys_err_t
md_cpu_start(uint32_t index)
{
    if (index >= SYS_MAX_CORES) {
        return SYS_ERR_NO_ENT;
    }

    /*
     * The entry point is this port's own, not the caller's: a core
     * arriving from firmware has no stack, no vector table and no
     * translation, and has to have all three before it can enter C.
     * boot.S enters cpu_secondary_entry once it has.
     */
    if (psci_call(PSCI_CPU_ON, index, (uint64_t)(uintptr_t)secondary_entry,
      0) != 0) {
        return SYS_ERR_NOT_SUPPORTED;
    }

    return SYS_OK;
}

void
md_cpu_fence(void)
{
    __asm__ volatile ("dmb ish" ::: "memory");
}

void
md_cpu_relax(void)
{
    __asm__ volatile ("yield" ::: "memory");
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
     * The affinity register, masked to the field this board uses. A
     * core here can read its own identity at any time, where the other
     * port cannot: mhartid is machine mode's and this is not.
     *
     * Masked to the affinity field this board numbers its cores in,
     * and not to the number of cores this build has room for. A core
     * outside that range has to arrive with the identity it really
     * has, so that claiming it is refused rather than folded onto a
     * core that already exists.
     */
    __asm__ volatile ("mrs %0, mpidr_el1" : "=r" (id));

    return (uint32_t)(id & 3u);
}
