/*
 * SPDX-License-Identifier: 0BSD OR Apache-2.0
 * Copyright (C) 2026 Jon Mayo <jon@rm-f.net>
 */

/*
 * Stopping the machine.
 *
 * QEMU's virt board emulates PSCI for the guest, so SYSTEM_OFF is enough to
 * end the run. Powering off rather than spinning is deliberate: a kernel
 * that has decided it cannot continue should end, so that whatever launched
 * it observes the exit rather than a hang.
 *
 * PSCI SYSTEM_OFF carries no status, so QEMU exits 0 whether the kernel
 * stopped deliberately or panicked. The RISC-V port has a finisher that
 * does carry a code and uses it. Anything checking this port automatically
 * has to read the console for "panic:" rather than trust the exit status.
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
