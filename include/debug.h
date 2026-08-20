/*
 * SPDX-License-Identifier: 0BSD OR Apache-2.0
 * Copyright (C) 2026 Jon Mayo <jon@rm-f.net>
 */

/*
 * Conditional debug output, over the console of section 22.4.
 *
 * A program defines DEBUG_CONSOLE before including this, or has its
 * Makefile do it, and its debug output exists. Without it the macros
 * expand to nothing: no call, no string in the image, no bytes spent on
 * diagnostics the build does not want. That is the point of a macro rather
 * than a function with a flag in it.
 *
 * This is diagnostics and not output. A program's real output goes through
 * write(), which today has nowhere to reach but the same console and after
 * M3 reaches a console service through a handle. What is here is the
 * category a build removes, so the two do not share a spelling.
 *
 * The kernel has its own two gates on the far side, per section 22.4, and
 * a call that reaches a kernel with the console dormant is refused. These
 * macros ignore the result: a diagnostic that fails is not an error worth
 * handling, and a program that behaved differently depending on whether
 * anyone was listening would be a program that cannot be debugged.
 */

#ifndef _DEBUG_H_
#define _DEBUG_H_

#include <os/sys.h>
#include <string.h>

#ifndef DEBUG_CONSOLE
#define DEBUG_CONSOLE 0
#endif

#if DEBUG_CONSOLE

#define DBG_WRITE(buf, len) ((void)sys_debug_write((buf), (len)))
#define DBG_PUTS(s)         do {                                        \
        static const char dbg_s_[] = (s);                               \
        (void)sys_debug_write(dbg_s_, (uint32_t)sizeof(dbg_s_) - 1);    \
        (void)sys_debug_write("\n", 1);                                 \
    } while (0)

#else

/*
 * The arguments are still mentioned, so that a debug-only variable does
 * not become unused when the output goes and the build starts warning
 * about code it is about to compile back in.
 */
#define DBG_WRITE(buf, len) ((void)sizeof((buf)), (void)sizeof((len)))
#define DBG_PUTS(s)         ((void)sizeof(s))

#endif

#endif /* _DEBUG_H_ */
