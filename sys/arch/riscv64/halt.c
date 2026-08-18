/*
 * SPDX-License-Identifier: 0BSD OR Apache-2.0
 * Copyright (C) 2026 Jon Mayo <jon@rm-f.net>
 */

/*
 * Stopping the machine.
 *
 * QEMU's virt board exposes the SiFive test finisher, a single MMIO word
 * that ends the run. Powering off rather than spinning is deliberate: a
 * kernel that has decided it cannot continue should end, so that whatever
 * launched it observes the exit rather than a hang.
 *
 * Unlike PSCI on the ARM side, the finisher carries an exit status, so a
 * failing halt is visible to a script and not only on the console.
 */

#include <sys/systm.h>

#include <stdint.h>

#define SIFIVE_TEST_ADDR    0x100000UL

#define FINISHER_PASS       0x5555u
#define FINISHER_FAIL       0x3333u
#define FINISHER_RESET      0x7777u

static void
finisher_write(uint32_t command)
{
    volatile uint32_t *finisher = (volatile uint32_t *)SIFIVE_TEST_ADDR;

    *finisher = command;
}

void
machine_halt(int status)
{
    if (status == 0) {
        finisher_write(FINISHER_PASS);
    } else {
        finisher_write(FINISHER_FAIL | ((uint32_t)status << 16));
    }

    /* Only reached if the finisher is absent. */
    for (;;) {
        __asm__ volatile ("wfi");
    }
}

void
machine_reset(void)
{
    finisher_write(FINISHER_RESET);

    for (;;) {
        __asm__ volatile ("wfi");
    }
}
