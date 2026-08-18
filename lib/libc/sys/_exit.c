/*
 * SPDX-License-Identifier: 0BSD OR Apache-2.0
 * Copyright (C) 2026 Jon Mayo <jon@rm-f.net>
 */

#include <sys/syscall.h>
#include <unistd.h>

#include "libc_syscall.h"

void
_exit(int status)
{
    __syscall(SYS_exit, status, 0, 0);
    for (;;) {
        /* The kernel does not return from exit. */
    }
}
