/*
 * SPDX-License-Identifier: 0BSD OR Apache-2.0
 * Copyright (C) 2026 Jon Mayo <jon@rm-f.net>
 */

#include <sys/syscall.h>
#include <unistd.h>

#include "libc_syscall.h"

int
close(int fd)
{
    return (int)__syscall(SYS_close, fd, 0, 0);
}
