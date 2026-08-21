/*
 * SPDX-License-Identifier: 0BSD OR Apache-2.0
 * Copyright (C) 2026 Jon Mayo <jon@rm-f.net>
 */

/*
 * A program that does nothing for a long time, so that something else can
 * interrupt it.
 *
 * Until milestone 3 stage 2 both ports entered user mode with interrupts
 * masked, which meant no program had ever been interrupted and the AArch64
 * lower-EL interrupt vector had never been entered. That is not a thing a
 * program can demonstrate about itself: what it can do is run long enough
 * for a timer armed before it started to expire while it is the thread on
 * the core, and exit normally afterwards.
 *
 * The count is a busy loop rather than a wait because a wait would leave
 * user mode, which is the one place this has to stay.
 */

#include <os/sys.h>

/*
 * Two million iterations took sixteen milliseconds on both boards under
 * emulation, against a timer armed one millisecond out, so the interrupt
 * lands with an order of magnitude to spare. Ten million keeps that
 * margin on a host fast enough to make two million disappear, and costs
 * eighty milliseconds of boot to do it.
 */
#define SPINS   10000000u

int
main(void)
{
    volatile unsigned int n = SPINS;

    while (n > 0) {
        n--;
    }

    return 0;
}
