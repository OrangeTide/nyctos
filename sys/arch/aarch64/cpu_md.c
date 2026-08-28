/*
 * SPDX-License-Identifier: 0BSD OR Apache-2.0
 *
 * PUBLIC DOMAIN (CC0-1.0)
 */

/*
 * Cores on this port.
 *
 * The platform holds the secondaries powered off and starts one on
 * request, through the same PSCI interface this port already uses to
 * power the machine down. So starting a core here is a call into
 * firmware, where the other port has no firmware and releases a hart
 * that was already spinning. Both end at the same place, which is a core
 * running kernel code with a stack of its own.
 *
 * The per-core pointer lives in TPIDR_EL1, which is exactly what the
 * architecture provides it for and which nothing here used before.
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
md_cpu_start(uint32_t index, uintptr_t entry)
{
    (void)entry;

    if (index >= SYS_MAX_CORES) {
        return SYS_ERR_NO_ENT;
    }

    /*
     * The entry point is this port's own, not the caller's: a core
     * arriving from firmware has no stack, no vector table and no
     * translation, and has to have all three before it can enter C.
     * What the caller asked for is where boot.S sends it afterwards.
     */
    if (psci_call(PSCI_CPU_ON, index, (uint64_t)(uintptr_t)secondary_entry,
      0) != 0) {
        return SYS_ERR_NOT_SUPPORTED;
    }

    return SYS_OK;
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
     */
    __asm__ volatile ("mrs %0, mpidr_el1" : "=r" (id));

    return (uint32_t)(id & 3u);
}
