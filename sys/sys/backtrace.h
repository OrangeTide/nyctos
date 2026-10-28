/* backtrace.h : the kernel stack unwinder.
 *
 * SPDX-License-Identifier: 0BSD OR Apache-2.0
 * Copyright (C) 2026 Jon Mayo <jon@rm-f.net>
 *
 * Walks the saved frame-pointer chain to the return addresses of the calls
 * on the stack. The kernel is built with frame pointers so the chain is
 * there to follow. The decode of one frame is per-architecture; the walk
 * over the chain is not.
 *
 */

#ifndef _SYS_BACKTRACE_H_
#define _SYS_BACKTRACE_H_

#include <stdbool.h>
#include <stdint.h>

/*
 * Decode one frame. Given a frame pointer, report the return address saved
 * in that frame and the caller's frame pointer, and whether the frame
 * decoded at all. The layout is the architecture's; aarch64 saves the pair
 * at the frame pointer, riscv64 just below it.
 */
bool md_frame_step(uintptr_t fp, uintptr_t *ra, uintptr_t *caller_fp);

/*
 * Fill out[] with up to max return addresses, innermost first, starting
 * from the caller of kbacktrace. Returns how many frames were found. The
 * walk stops at a frame pointer that is zero, not above the last, or
 * misaligned, so a broken chain ends the walk rather than running off.
 */
unsigned int kbacktrace(uintptr_t *out, unsigned int max);

#endif /* _SYS_BACKTRACE_H_ */
