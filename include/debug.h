/* debug.h : conditional diagnostic output macros.
 *
 * SPDX-License-Identifier: 0BSD OR Apache-2.0
 * Copyright (C) 2026 Jon Mayo <jon@rm-f.net>
 *
 * DBG_WRITE and DBG_PUTS send diagnostics to the debug console when
 * DEBUG_CONSOLE is nonzero. When it is zero, both expand to a sizeof()
 * expression that emits no code. Diagnostic output is separate from the
 * program output that write() sends to the console connection. Included
 * by the console, fs_batch, spawner, and threads programs.
 *
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
