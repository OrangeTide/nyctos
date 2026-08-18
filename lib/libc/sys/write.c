/*
 * SPDX-License-Identifier: 0BSD OR Apache-2.0
 * Copyright (C) 2026 Jon Mayo <jon@rm-f.net>
 */

#include <sys/syscall.h>
#include <unistd.h>

#include "libc_syscall.h"

ssize_t
write(int fd, const void *buf, size_t nbytes)
{
    return (ssize_t)__syscall(SYS_write, fd, (long)buf, (long)nbytes);
}
