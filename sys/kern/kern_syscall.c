/*
 * SPDX-License-Identifier: 0BSD OR Apache-2.0
 * Copyright (C) 2026 Jon Mayo <jon@rm-f.net>
 */

/*
 * System call dispatch. The machine-dependent trap stubs decode the
 * hardware's idea of a system call and land here with the arguments
 * already in the C calling convention.
 */

#include <sys/syscall.h>
#include <sys/systm.h>
#include <sys/vfs.h>

#define ENOENT 2
#define EBADF 9
#define ENOSYS 78

static long
sys_write(int fd, const char *buf, size_t len)
{
    if (fd != 1 && fd != 2) {
        return -EBADF;
    }
    cnwrite(buf, len);
    return (long)len;
}

static long
sys_open(const char *path)
{
    int fd = file_open(path);

    return fd < 0 ? -ENOENT : fd;
}

static long
sys_read(int fd, void *buf, size_t len)
{
    long n = file_read(fd, buf, len);

    return n < 0 ? -EBADF : n;
}

static long
sys_close(int fd)
{
    return file_close(fd) < 0 ? -EBADF : 0;
}

static void
sys_exit(int status)
{
    (void)status;
    cnputs("init: /bin/motd (pid 1) exited; nothing left to run; system halted\n");
    for (;;) {
        /* Nothing left to do. */
    }
}

long
syscall_handler(long num, long a0, long a1, long a2)
{
    switch (num) {
    case SYS_write:
        return sys_write((int)a0, (const char *)a1, (size_t)a2);
    case SYS_open:
        return sys_open((const char *)a0);
    case SYS_read:
        return sys_read((int)a0, (void *)a1, (size_t)a2);
    case SYS_close:
        return sys_close((int)a0);
    case SYS_exit:
        sys_exit((int)a0);
        /* NOTREACHED */
    default:
        return -ENOSYS;
    }
}

void
trap_fatal(unsigned long cause, unsigned long pc)
{
    /*
     * Once processes exist, an unhandled trap terminates the faulting
     * process and the system carries on; that is what section 16 of
     * design.md means by fatal by default. There is nothing but the kernel
     * to fault right now, so every trap is a kernel bug.
     */
    panic("unhandled trap: cause=0x%lx pc=0x%lx", cause, pc);
}
