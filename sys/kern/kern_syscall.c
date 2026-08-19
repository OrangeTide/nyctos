/*
 * SPDX-License-Identifier: 0BSD OR Apache-2.0
 * Copyright (C) 2026 Jon Mayo <jon@rm-f.net>
 */

/*
 * The trap path.
 *
 * Section 23.1: one entry point carrying a pointer to an operation
 * descriptor and a count, and no system call numbers. The count is one
 * until the submission engine of section 17 exists.
 *
 * The machine-dependent stubs land here with the arguments already in the C
 * calling convention. Nothing reaches this yet, because no user code runs:
 * the first process arrives in stage 9, and until then every caller of
 * sys_op_exec is kernel code calling it directly. That is the same entry
 * point and the same implementation, which is the point of section 23.1,
 * but it does mean the trap edge itself is unexercised.
 */

#include <sys/op.h>
#include <sys/systm.h>

long
syscall_handler(struct sys_op *op, unsigned long count)
{
    /*
     * The count is one until the submission engine of section 17 exists.
     * Rejecting anything else here rather than ignoring it means a caller
     * built against that engine fails loudly on a kernel without it.
     */
    if (count != 1) {
        return SYS_ERR_INVALID_ARG;
    }

    return sys_op_exec(op);
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
