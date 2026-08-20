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
 * The machine-dependent stubs land here with the arguments already in the
 * C calling convention.
 *
 * What is checked here, and what is not, is section 23.2 and is worth
 * stating in the file rather than leaving to the reader. The count is
 * checked against its bound. The opcode is checked against the table, and
 * each operation checks its own argument length, its handles and their
 * rights, because all of those are questions about values the kernel
 * holds.
 *
 * The pointer is not checked at all. There is one flat address space on
 * this tier, so there is nothing to ask whether a range is mapped into: a
 * descriptor pointer from a user program is dereferenced, and so is every
 * arg_ptr it carries. That is section 6's line about handles enforcing
 * modularity and not security, arriving at the system call path. A Tier 2
 * port is where step 2 of section 23.2 acquires an answer.
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
