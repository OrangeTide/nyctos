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
#include <sys/page.h>
#include <sys/proc.h>
#include <sys/thread.h>
#include <sys/vm.h>

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

    /*
     * Section 23.2 step 2: the descriptor array has to be memory the
     * caller can reach, or the call returns SYS_ERR_FAULT having executed
     * nothing. This is the first dereference of an address a program
     * chose, and without the check it is the kernel faulting on a number
     * a program made up.
     *
     * The array is writable as well as readable, because the kernel puts
     * each entry's status back into it per section 17.6. One check covers
     * both: what a program can reach in a descriptor is its own memory,
     * and its own memory is writable.
     */
    if (!proc_reaches(proc_current(), (uintptr_t)op,
      count * sizeof(*op))) {
        return SYS_ERR_FAULT;
    }

    {
        long r = sys_op_exec(op);
        struct proc *p = proc_current();

        /*
         * And out, if this process has been killed. Here because it
         * is the one place a thread is certainly in the kernel with
         * nothing of its own left on the stack: unwinding a thread
         * from wherever it happened to be would mean knowing what
         * every kernel path holds, and this knows that it holds
         * nothing.
         *
         * Every blocked thread of a killed process is released into
         * this line, because being blocked is being inside a system
         * call. A thread computing in user mode reaches it at its
         * next call, and one that never calls in is not stopped,
         * which section 10.5 does not ask for: what it asks is that
         * the unblocking be prompt.
         */
        if (p != NULL && p->p_dying) {
            thread_exit(p->p_kill_code);
        }

        return r;
    }
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
 * A synchronous exception taken while a program was running.
 *
 * A page fault its pager can answer is the one case that comes back
 * here: section 15.3 sends it as a message, the reply carries the page,
 * and the instruction is tried again. Everything else ends the thread,
 * per section 16.2, and does not return.
 */
void
trap_user(unsigned long cause, unsigned long pc, unsigned long addr)
{
    uint32_t kind = md_fault_kind(cause);
    uintptr_t where = md_fault_has_addr(cause) ? (uintptr_t)addr : 0;

#if SYS_TIER == 2
    /*
     * A page fault the tables can answer is not a page fault.
     *
     * One port distinguishes an address nothing describes from one
     * described without the permission the access needed; the other has
     * a single cause code for both, and says so. Neither difference is
     * worth pushing into the ports, because the tables hold the answer
     * and the kernel can read them: an address that resolves was there,
     * so what the hardware objected to was the access rather than the
     * absence.
     *
     * This is what keeps a pager from being asked about memory that is
     * present and refused, which is a program touching what it lent out
     * or what it was never given.
     */
    if (kind == SYS_FAULT_PAGE) {
        struct thread *t = thread_current();
        uintptr_t pa;

        if (t != NULL && md_lookup(proc_as(t->t_proc),
          where & ~(uintptr_t)PAGE_MASK, &pa) == SYS_OK) {
            kind = SYS_FAULT_ACCESS;
        }
    }

    /*
     * And if it is still a page fault, ask the pager. This is the only
     * path in the kernel that returns from a fault: the page is there
     * now and the instruction the thread stopped on is tried again.
     *
     * What the access was is what the port said the instruction was
     * doing, and section 15.1 has a pager supply read-only pages only,
     * so a fault that wanted to write is refused rather than asked
     * about.
     */
    if (kind == SYS_FAULT_PAGE &&
      pager_fault(where, (uintptr_t)pc, md_fault_access(cause))) {
        return;
    }
#endif

    thread_fault(kind, where, (uintptr_t)pc);
}
