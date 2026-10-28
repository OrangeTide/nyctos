/* subr_backtrace.c : the machine-independent stack unwinder.
 *
 * SPDX-License-Identifier: 0BSD OR Apache-2.0
 * Copyright (C) 2026 Jon Mayo <jon@rm-f.net>
 *
 * Walks the saved frame-pointer chain and collects the return address of
 * each frame. One frame is decoded by the architecture; this follows the
 * chain and guards it, so a frame pointer that is zero, not above the one
 * before it, or misaligned ends the walk rather than reading somewhere it
 * should not.
 *
 */

#include <sys/backtrace.h>
#include <sys/config.h>

#include <stdint.h>

unsigned int
kbacktrace(uintptr_t *out, unsigned int max)
{
    uintptr_t fp = (uintptr_t)__builtin_frame_address(0);
    uintptr_t base = fp;
    unsigned int n = 0;

    /*
     * The frame pointer stays within one stack above where the walk began,
     * so a chain that does not terminate cleanly stops at the stack's edge
     * rather than reading an address that is not stack. With the per-frame
     * alignment and the requirement that each caller's frame is above this
     * one, every address read is inside the stack this call runs on.
     */
    while (n < max && fp != 0 && (fp & (sizeof(uintptr_t) - 1)) == 0 &&
      fp - base < SYS_KSTACK_SIZE) {
        uintptr_t ra = 0, caller = 0;

        if (!md_frame_step(fp, &ra, &caller)) {
            break;
        }
        if (ra == 0) {
            break;
        }
        out[n++] = ra;

        /* The stack grows down, so each caller's frame is above this one. */
        if (caller <= fp) {
            break;
        }
        fp = caller;
    }

    return n;
}
