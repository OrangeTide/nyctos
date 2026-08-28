/*
 * SPDX-License-Identifier: 0BSD OR Apache-2.0
 *
 * PUBLIC DOMAIN (CC0-1.0)
 */

/*
 * Cores on this port.
 *
 * There is no firmware under this kernel, so there is no SBI and no hart
 * state management call to start a hart with. Every hart arrives at the
 * reset vector together and every one but the first parks in boot.S
 * waiting for a word to change. Starting a core is therefore writing
 * that word, which is less than a port with firmware has to do and is
 * the whole of the difference.
 *
 * The per-core pointer lives in tp. Supervisor mode owns it, the C code
 * here never had a thread pointer of its own, and reading it is one
 * instruction, which is what section 11.4 needs: finding out what this
 * core is doing must not mean agreeing with another one.
 */

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
