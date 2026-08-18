/*
 * SPDX-License-Identifier: 0BSD OR Apache-2.0
 * Copyright (C) 2026 Jon Mayo <jon@rm-f.net>
 */

/*
 * The trap path.
 *
 * The machine-dependent stubs decode the hardware's idea of a system call
 * and land here. There is nothing to dispatch to yet: the numbered calls
 * that used to live here belonged to the banner demo and went with it, and
 * the interface that replaces them is the operation descriptor of section
 * 23, which arrives in stage 4 along with the handle table it operates on.
 *
 * Until then no user code runs, so nothing can reach this.
 */

#include <sys/systm.h>

#define ENOSYS 78

long
syscall_handler(long num, long a0, long a1, long a2)
{
    (void)num;
    (void)a0;
    (void)a1;
    (void)a2;

    return -ENOSYS;
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
