/* spin.c : a program that busy-loops long enough to be interrupted.
 *
 * SPDX-License-Identifier: 0BSD OR Apache-2.0
 * Copyright (C) 2026 Jon Mayo <jon@rm-f.net>
 *
 * It counts down a fixed number of iterations in user mode and then exits
 * with status 0. The count is long enough that a timer armed before the
 * program starts expires while it runs. It busy-loops rather than waiting, so
 * it never leaves user mode.
 *
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
