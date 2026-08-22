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
#include <sys/thread.h>

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
trap_fatal(unsigned long cause, unsigned long pc, unsigned long addr)
{
    struct thread *t = thread_current();

    /*
     * A synchronous exception the kernel took itself. Section 16.2 is
     * about a thread running a program: it dies and the system carries
     * on, which trap_user does. There is nobody to report this one to
     * and nothing sensible to do with a kernel whose own instruction
     * did not run, so it stops the machine and says everything it
     * knows.
     */
    panic("unhandled trap in %s: %s, cause=0x%lx pc=0x%lx addr=0x%lx "
      "space=0x%lx", t != NULL ? t->t_name : "?",
      fault_name(md_fault_kind(cause)), cause, pc, addr,
      md_current_space());
}

/*
 * A program faulted.
 *
 * Section 16.2: the thread dies, the system carries on, and what killed
 * it is in its status for whoever is watching. Everything that makes
 * that work was built by exit: the completion timeline advances, a
 * joiner is released, donated priority unwinds. A fault is an exit whose
 * code the thread did not choose, which is why this is four lines rather
 * than a mechanism.
 *
 * The kernel says nothing here. Whoever holds the process is the one
 * that reports this, because the report needs a name for the program and
 * a place to put it; and a program nobody holds does not go quiet, it
 * halts the machine with the fault named, per the unwatched exit of
 * section 24.3's placeholder.
 */
void
trap_user(unsigned long cause, unsigned long pc, unsigned long addr)
{
    uint32_t kind = md_fault_kind(cause);
    uintptr_t where = md_fault_has_addr(cause) ? (uintptr_t)addr : 0;

    thread_fault(kind, where, (uintptr_t)pc);
}
