/* _exit.c : terminates the calling thread with a status.
 *
 * SPDX-License-Identifier: 0BSD OR Apache-2.0
 * Copyright (C) 2026 Jon Mayo <jon@rm-f.net>
 *
 * _exit passes its status to sys_thread_exit. The kernel does not return from
 * that call, and the trailing loop is a guard for it.
 *
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
