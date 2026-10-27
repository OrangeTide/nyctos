/* halt.c : RISC-V machine halt through the SiFive test finisher.
 *
 * SPDX-License-Identifier: 0BSD OR Apache-2.0
 * Copyright (C) 2026 Jon Mayo <jon@rm-f.net>
 *
 * machine_halt writes the pass code to the SiFive test finisher at
 * 0x100000 for status zero. Any other status writes the fail code with
 * the status in the upper bits, so the host sees it as the exit status.
 * machine_reset writes the reset code. If the write returns, the hart
 * waits in wfi.
 *
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
