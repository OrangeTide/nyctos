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

#include <stdbool.h>

#include <sys/cpu.h>
#include <sys/measure.h>
#include <sys/lock.h>
#include <sys/op.h>
#include <sys/systm.h>
#include <sys/page.h>
#include <sys/proc.h>
#include <sys/thread.h>
#include <sys/vm.h>

static long syscall_body(struct sys_op *op, unsigned long count);

/*
 * Every way into this kernel is one of four functions the port's
 * exception vectors call, and each one is a pair of these. A thread
 * that blocks in here is switched away from still holding the lock,
 * and the thread that runs next releases it on its own way out.
 */
long
syscall_handler(struct sys_op *op, unsigned long count)
{
    intr_state_t s = klock_enter();
    long r = syscall_body(op, count);

    klock_exit(s);

    /*
     * And if this thread got here because an interrupt made it
     * runnable, that is section 13.3's second term closing: the
     * distance from the interrupt being taken to its program running.
     */
    irq_dispatch_arrived(thread_current());

    /*
     * The instruction that returns to user mode turns interrupts back
     * on without going through the restore, so the region has to be
     * closed here. Left open it runs through everything the program
     * then does, and the figure stops being about the kernel.
     */
    intr_region_end((uintptr_t)__builtin_return_address(0));

    return r;
}

static long
syscall_body(struct sys_op *op, unsigned long count)
{
    /*
     * A submission carries at least one entry and at most the bound of
     * section 17.2. Zero is refused rather than treated as nothing to
     * do: a caller that computed a count of zero has a bug, and a
     * system call that succeeds without doing anything hides it.
     */
    cpu_self()->cp_syscalls++;

    if (count == 0 || count > SYS_BATCH_MAX_ENTRIES) {
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
    cpu_self()->cp_checks++;
    if (!proc_reaches(proc_current(), (uintptr_t)op,
      count * sizeof(*op))) {
        return SYS_ERR_FAULT;
    }

    /*
     * The check that a killed process leaves the kernel is inside the
     * loop below rather than after it. It is there because a thread is
     * certainly in the kernel with nothing of its own left on the
     * stack at an entry boundary: unwinding a thread from wherever it
     * happened to be would mean knowing what every kernel path holds,
     * and an entry boundary knows that it holds nothing.
     *
     * Every blocked thread of a killed process is released into that
     * line, because being blocked is being inside a system call. A
     * thread computing in user mode reaches it at its next call, and
     * one that never calls in is not stopped, which section 10.5 does
     * not ask for: what it asks is that the unblocking be prompt.
     */

    /*
     * Every entry checked before any entry runs, per section 17.2. A
     * batch that cannot run must not half-run: the alternative is a
     * caller whose first two operations happened and whose third was
     * refused for a reason it could have been told before anything
     * did.
     *
     * What can be checked here is what does not need the operation:
     * that the opcode exists, and that an argument structure is memory
     * this process can reach. Everything else is the operation's own
     * business and section 17.2 says so.
     */
    {
        unsigned long i;

        for (i = 0; i < count; i++) {
            sys_err_t bad = SYS_OK;

            cpu_self()->cp_checks++;

            if (op[i].opcode >= SYS_OP_COUNT) {
                bad = SYS_ERR_INVALID_ARG;
            } else if (op[i].arg_ptr != 0 &&
              !proc_reaches(proc_current(), op[i].arg_ptr,
                op[i].arg_len)) {
                /*
                 * The same condition sys_op_exec applies, and it has
                 * to be the same one. A null pointer with a length is
                 * a malformed descriptor rather than an unreachable
                 * one, and the operations answer that with
                 * SYS_ERR_INVALID_ARG; testing the length here instead
                 * of the pointer turned that answer into FAULT, which
                 * is a decision sys_op_exec had already made and
                 * written down.
                 *
                 * So this check is a copy, deliberately. Section 17.2
                 * wants every entry checked before any entry runs, and
                 * the operation checks again when it runs, so an entry
                 * is reach-checked twice. That is the cost of not
                 * half-running a batch, and it is a number stage four
                 * has to account for rather than a thing to be quietly
                 * pleased about.
                 */
                bad = SYS_ERR_FAULT;
            }

            if (bad != SYS_OK) {
                /*
                 * The entry that was refused says so, per section
                 * 17.6, and the others are left as the caller wrote
                 * them because they were never considered. An entry
                 * refused before it ran still has to report why it was
                 * refused: the check that caught this was already in
                 * the tree, in a program that submits a descriptor
                 * pointing at memory it does not own and asserts that
                 * both the call and the entry say FAULT.
                 */
                op[i].status = bad;
                return bad;
            }
        }
    }

    {
        struct proc *p;
        unsigned long i;
        long r = SYS_OK;
        bool cancelling = false;

        for (i = 0; i < count; i++) {
            bool linked = (op[i].flags & SYS_OP_LINK) != 0;
            long one;

            /*
             * A batch entry boundary, per section 13.3 and section
             * 17.6. The batch is preemptible here and an entry is not,
             * which is why the bound above does not appear in the
             * latency figures and the cost of one entry does.
             */
            preempt_point();

            if (cancelling) {
                /*
                 * An earlier entry of this chain failed, so this one
                 * does not run and says so. Section 17.5: the first
                 * failure cancels the remainder of that chain.
                 */
                op[i].status = SYS_ERR_CANCELED;
                one = SYS_OK;
            } else {
                one = sys_op_exec(&op[i]);

                /*
                 * A failure cancels what is left of this chain, and
                 * only this chain. An entry that is not linked to the
                 * next one has nothing after it to cancel.
                 */
                if (one != SYS_OK && linked) {
                    cancelling = true;
                }
            }

            /*
             * The chain ends at the first entry without the flag, so
             * whatever comes after starts a new one and is unaffected
             * by this one having failed.
             */
            if (!linked) {
                cancelling = false;
            }

            /*
             * And out here if this process has been killed, which is
             * the entry boundary section 17.6 makes the batch
             * preemptible at. Checking only after the whole array
             * would let a killed thread run up to fifteen more
             * operations first, and section 10.5 asks for the
             * unblocking to be prompt rather than eventual.
             *
             * An entry boundary is also the one place a thread is
             * certainly in the kernel with nothing of its own left on
             * the stack. Unwinding from wherever it happened to be
             * would mean knowing what every kernel path holds, and
             * this knows that it holds nothing.
             */
            p = proc_current();
            if (p != NULL && p->p_dying) {
                thread_exit(p->p_kill_code);
            }

            /*
             * The call reports the first entry that failed, and every
             * entry reports its own status in the array. With one
             * entry the two are the same thing, which is what every
             * caller written before this milestone assumes.
             */
            if (one != SYS_OK && r == SYS_OK) {
                r = one;
            }
        }

        return r;
    }
}

void
trap_fatal(unsigned long cause, unsigned long pc, unsigned long addr)
{
    struct thread *t;

    /*
     * Taken like any other entry, though nothing here gives it back.
     * A core that stops the machine while another is spinning for this
     * lock is a core that stops the machine, which is what was wanted.
     */
    (void)klock_enter();
    t = thread_current();

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
static void trap_user_body(unsigned long cause, unsigned long pc,
  unsigned long addr);

void
trap_user(unsigned long cause, unsigned long pc, unsigned long addr)
{
    intr_state_t s = klock_enter();

    trap_user_body(cause, pc, addr);
    klock_exit(s);
    intr_region_end((uintptr_t)__builtin_return_address(0));
}

static void
trap_user_body(unsigned long cause, unsigned long pc, unsigned long addr)
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
