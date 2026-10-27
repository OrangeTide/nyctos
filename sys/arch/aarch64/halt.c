/* halt.c : AArch64 machine halt and reset through PSCI.
 *
 * SPDX-License-Identifier: 0BSD OR Apache-2.0
 * Copyright (C) 2026 Jon Mayo <jon@rm-f.net>
 *
 * machine_halt ignores its status argument and calls PSCI SYSTEM_OFF
 * through hvc #0. machine_reset calls SYSTEM_RESET the same way. PSCI
 * carries no exit status, so QEMU exits 0 whether the kernel halted
 * normally or panicked, and a script must read the console for "panic:".
 * If the call returns, machine_halt waits in wfi.
 *
 */

#include <sys/systm.h>

#include <stdint.h>

#define PSCI_SYSTEM_OFF     0x84000008u
#define PSCI_SYSTEM_RESET   0x84000009u

static void
psci_call(uint32_t function)
{
    register unsigned long x0 __asm__("x0") = function;

    __asm__ volatile ("hvc #0" : "+r" (x0) : : "memory");
}

void
machine_halt(int status)
{
    (void)status;

    psci_call(PSCI_SYSTEM_OFF);

    /*
     * Only reached if the platform does not implement PSCI, in which case
     * there is nothing further to try. Wait rather than spin hot.
     */
    for (;;) {
        __asm__ volatile ("wfi");
    }
}

void
machine_reset(void)
{
    psci_call(PSCI_SYSTEM_RESET);

    for (;;) {
        __asm__ volatile ("wfi");
    }
}
