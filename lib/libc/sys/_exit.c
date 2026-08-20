/*
 * SPDX-License-Identifier: 0BSD OR Apache-2.0
 * Copyright (C) 2026 Jon Mayo <jon@rm-f.net>
 */

#include <os/sys.h>
#include <unistd.h>

void
_exit(int status)
{
    sys_thread_exit(status);
    for (;;) {
        /* The kernel does not return from exit. */
    }
}
