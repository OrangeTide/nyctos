/* proc.h : process structure and process operations.
 *
 * SPDX-License-Identifier: 0BSD OR Apache-2.0
 * Copyright (C) 2026 Jon Mayo <jon@rm-f.net>
 *
 * Declares struct proc, which holds the process identifier, name, handle table,
 * namespace, startup record, command line and cohort. Also declares the process
 * lifecycle, reference, reap and address-space functions. Included by the
 * object header, the cohort, thread, pager, grant, channel, syscall, operation,
 * CPU and init sources.
 *
 */

#ifndef _SYS_PROC_H_
#define _SYS_PROC_H_

#include <sys/cohort.h>
#include <sys/connection.h>
#include <sys/handle.h>
#include <sys/pool.h>
#include <sys/startup.h>
#include <sys/timeline.h>
#include <sys/vm.h>

#include <stdbool.h>

struct proc {
    sys_id_t            p_id;
    /*
     * Copied, not pointed at. The name a spawn operation carries lives
     * in the caller's argument structure, which is a stack frame in a
     * program, and the kernel says this name again when the process
     * exits. A pointer would be dangling by then, which is what it was
     * the first time this ran: the exit line printed sixteen bytes of
     * whatever the frame had become.
     */
    char                p_name[SYS_IMAGE_NAME_MAX + 1];
    struct handle_table p_handles;

    /*
     * The startup record, and the table it points at. Both live in the
     * process rather than in the child's memory because Tier 0 has no
     * address space to place them in.
     */
    sys_startup_t       p_startup;
    sys_handle_t        p_ns[SYS_NS_WORDS];

    /*
     * Where each of those handles is mounted. Copied
     * for the same reason the command line is: the array a caller builds
     * is a stack frame in a program, and the child reads it after that
     * frame is gone. An entry that is only a role has an empty slot.
     */
    char                p_ns_names[SYS_NS_NAME_BYTES];

    /*
     * The command line, copied. The kernel is the loader, so the strings
     * live in the process object beside the record that points at
     * them. Copied rather than pointed at for the reason every other
     * copy here exists: the array a caller passed is a stack frame in
     * a program, and the child reads it after that frame is gone.
     */
    char                p_args[SYS_ARG_BYTES];
    uintptr_t           p_argv[SYS_MAX_ARGS + 1];

    /*
     * The cohort this process is in, or NULL for a process in none.
     * Fixed when the process is created, from a
     * table the kernel was built with, and never afterwards: there is
     * no operation that joins one.
     *
     * A member does not own the address space below. It runs in its
     * cohort's, which proc_as() is what knows.
     */
    struct cohort      *p_cohort;

#if SYS_TIER == 2
    /*
     * This process's own address space, used when it is in no cohort,
     * and where its startup record landed in whichever space it runs
     * in. Tier 0 has neither: there is one address space and the
     * record lives in this object, which a program can read because
     * nothing stops it.
     */
    struct addrspace    p_as;

    /*
     * How much of the slot the loader left to the pager: the leading
     * pages that hold the image, which a process with
     * a pager is given from a grant on a fault rather than at load.
     *
     * A boundary and not a flag, because a reap needs the inverse of
     * what proc_load decided page by page. Frames the loader allocated
     * are this process's to give back. Frames a pager supplied are its
     * grant's, mapped here at an address the pager did not choose, and
     * giving those back would free memory another process still holds.
     */
    size_t              p_pager_bytes;

    /*
     * How many frames the loader mapped for this process. Recorded
     * because the number cannot always be recovered later: a cohort
     * member whose place moved on has had its mappings written over.
     * The answer is what the image needed rather than a slot, so
     * counting it here is the only place it is known.
     */
    uint32_t            p_own_pages;

    /*
     * The layout the archive carried for this image, kept because a
     * page's permissions are a property of where it sits and more than
     * the loader needs to know them: a buffer lent to a server and
     * given back has to come back as what it was, and what it was is
     * this.
     */
    uint32_t            p_rw_bytes;
    uint32_t            p_mem_bytes;

    /*
     * Where the system page is mapped in this process,
     * as an offset into its slot, or zero for a process that has none.
     *
     * Kept because the reap has to skip it. One frame serves every
     * process, so a process giving back everything mapped in its slot
     * would give back the page the next one is still reading.
     */
    uint32_t            p_syspage_off;
#endif
    uintptr_t           p_startup_va;

    /*
     * The completion timeline, embedded rather than drawn from the
     * pool, and the status a joiner reads once it has advanced. A
     * process finishes when its last thread does, and the code it
     * reports is that thread's: with one thread per process that is the
     * program's exit status. Which thread speaks for a process with
     * several is not settled.
     */
    struct timeline     p_completion;
    uint32_t            p_nthreads;


    /*
     * What ended the thread whose exit finished this process, if the
     * hardware did. A supervisor holding a process
     * handle reads the fault here rather than having to have held a
     * thread handle it never saw.
     */
    uint32_t            p_fault;
    uintptr_t           p_fault_addr;
    uintptr_t           p_fault_pc;


    /*
     * The pager this process faults to, and what it
     * has cost. The connection is resolved at spawn from the handle the
     * namespace carried, because the kernel is what sends the fault and
     * a handle means something only in the table that holds it.
     *
     * NULL is the ordinary case and means a fault is fatal: there is
     * nobody to ask, which is the case for every process that is not on
     * a paging path.
     */
    struct connection  *p_pager;
    uint64_t            p_faults;

    /*
     * Killed. Its threads are unblocked at once and
     * each leaves at its next return from the kernel, because that is
     * the point where a thread can be stopped without unwinding it
     * from wherever it happens to be.
     */
    bool                p_dying;
    int32_t             p_kill_code;

    /*
     * Cores poked by the kill: one per thread of
     * this process that was running when it was killed. Zero for a
     * kill of something that is not running, which is every kill on a
     * single core.
     */
    uint64_t            p_kill_pokes;

    /*
     * Entries into the kernel this process made while marked for
     * death. The chances a kill had to take
     * effect, counted from every way in and not only the one that
     * looks for the mark.
     */
    uint64_t            p_dying_entries;

    /*
     * Memory-locked. Everything on a paging path is locked, so a fault in
     * one is fatal rather than recoverable: it is a configuration
     * mistake, and a system that deadlocks in the field is worse than
     * one that stops at integration.
     */
    bool                p_locked;

    /* Where a user program starts, and how it finished. */
    uintptr_t           p_entry;
    int32_t             p_exit_code;
    bool                p_exited;

    /*
     * Whether anything took this process's completion timeline. An exit
     * nobody asked about halts the system rather than passing unnoticed,
     * which is what stands in for supervision until a parent process can
     * make the decision.
     */
    bool                p_watched;

    /*
     * What still makes this process a thing that can be asked about:
     * handles naming it, plus its own threads that have not been
     * reaped. The slot goes back to the pool when this reaches zero
     * and the process has finished, and not when it finishes.
     *
     * The exit is not the event. A status stays
     * readable through a handle after the object has ended, so a
     * process that ended is not a process nobody can ask about; it
     * becomes one when the last thing that could ask lets go.
     */
    /*
     * How many times this process has crossed the trap edge, which is
     * the cost a submission batch exists to reduce. Per process because
     * that is the unit a comparison of two clients needs: the machine-wide count
     * includes whatever else was running, and on two cores that is not
     * a small difference.
     */
    uint64_t            p_submits;

    /*
     * And how much checking those crossings cost it. The pair is what a
     * submission form is weighed on: a form that makes
     * fewer calls but validates more per call has moved the cost
     * rather than removed it.
     */
    uint64_t            p_checks;

    uint32_t            p_refs;

    /*
     * Its threads, newest first. Cancellation needs this, and killing a
     * process needs it more: termination unblocks every thread of the
     * target exactly as if each had been cancelled, and there is no
     * way to do that to a set nothing can enumerate.
     */
    struct thread      *p_threads;
};

/*
 * Build the process the kernel's own threads belong to, with no children yet.
 */
struct proc *proc_bootstrap(const char *name);

/* The process the running thread belongs to. */
struct proc *proc_current(void);

/*
 * The kernel's own process, which every core reaches: a core other than
 * the first bootstraps a context of its own to dispatch away from, and
 * that context belongs to no program.
 */
struct proc *proc_kernel(void);

/*
 * Create a process, move the listed handles into it, and start a thread in
 * it. Authority flows downward: the child reaches exactly what is placed
 * here and the kernel grants nothing on request, so an empty list is a
 * process that can do nothing at all.
 */
struct proc *proc_spawn(const char *name, uint8_t prio,
  void (*entry)(void *), const sys_ns_spec_t *ns, uint32_t nns);

/*
 * The same, for a program in the boot archive rather than a function in
 * the kernel. The image is copied to the address it was linked for and a
 * thread is started that drops into it.
 */
struct proc *proc_spawn_image(const char *image, uint8_t prio,
  const sys_ns_spec_t *ns, uint32_t nns, const char *const *argv,
  uint32_t argc);

/*
 * The completion timeline, and the record that someone is watching.
 * Taking the timeline is what marks the process watched: an exit nobody
 * asked about still halts the machine, but the question
 * is now answered by whether anyone holds the thing that reports it
 * rather than by a flag the kernel sets on its own behalf.
 *
 * It cannot fail. The timeline is part of the process.
 */
/*
 * Spawn on behalf of a program. Everything that can fail happens before
 * anything that cannot be undone, so a caller that gets an error back
 * has a table and a set of pools exactly as they were.
 */
/*
 * Add a thread to a process. The stack is the
 * caller's, because the kernel allocates no user memory at any tier.
 */
sys_err_t proc_thread_create(struct proc *p, uintptr_t entry,
  uintptr_t uarg, uintptr_t stack, uint8_t prio,
  sys_handle_t *out_handle);

sys_err_t proc_spawn_op(struct proc *parent, const char *image,
  uint8_t prio, const sys_ns_spec_t *ns, uint32_t nns,
  const char *const *argv, uint32_t argc, sys_handle_t *out_handle);

void proc_watch(struct proc *p);

/*
 * End a process: unblock every thread of it exactly
 * as if each had been cancelled, and have each leave at its next return
 * from the kernel. Returns SYS_ERR_WOULDBLOCK for a process that has
 * already finished, which is not the same as having ended one.
 */
sys_err_t proc_kill(struct proc *p, int32_t code);

/*
 * Leave, if this thread's process has been killed. Called on the way
 * back to user mode from a system call, an interrupt and a fault, with
 * the kernel lock held; does not return in that case.
 */
void proc_exit_if_dying(void);

/*
 * What killing a process costs, as counts. See the definitions.
 */
extern uint64_t proc_kills;
extern uint64_t proc_kill_pokes;

/*
 * Threads ended on the way into user mode for the first time, split by
 * which trampoline they were on: a program's own first thread, and a
 * thread the program made. Both paths run the same check, and the two
 * are counted apart rather than together.
 */
extern uint64_t proc_unrun_programs;
extern uint64_t proc_unrun_threads;

/*
 * And how many of those found this core already holding the kernel
 * lock, which is a thread first dispatched by a core that was inside
 * the kernel. The branch exists because nothing forbids it; the count
 * says whether anything does it.
 */
extern uint64_t proc_unrun_locked;

/*
 * Threads asked for in a process that was already over, and refused.
 * A count of the times the set of threads a kill has to reach was
 * stopped from growing after the kill was written, and of the times a
 * process that had ended on its own was stopped from gaining a thread
 * it could not report.
 */
extern uint64_t proc_creates_refused;
extern uint64_t proc_creates_unmarked;

/*
 * The threads a kill had to reach, and the ones that left by the
 * third of the three doors.
 *
 * A kill closes the set of threads it has to reach at the moment it
 * writes the mark, and every thread in that set leaves the system by
 * exactly one of three places: the trampoline it had not yet been
 * dispatched to, the boundary between two entries of a submission,
 * or the return from a trap it was interrupted by. The first two are
 * counted where they happen. This is the third, and the total is
 * what says the three are all of them.
 */
extern uint64_t proc_reached;
extern uint64_t proc_exits_while_dying;
extern uint64_t proc_exits_own_way;
extern uint64_t proc_ended_at_trap;

/*
 * Threads stopped at an entry boundary because their process had
 * been killed, split by which boundary.
 *
 * Before the first entry of a submission is the point where a thread
 * that arrived at the kernel already marked is caught, which is a mark
 * written while it was in user mode on another core. Before a later
 * entry is the point after the entry before it, and after a batch is
 * the point after the last entry.
 */
extern uint64_t proc_stopped_before_first;
extern uint64_t proc_stopped_before_later;
extern uint64_t proc_stopped_after_batch;

/*
 * Whether a range of addresses is memory this process owns. A buffer
 * descriptor a program passes is checked against this
 * before anything reads it: a number is not a right, and the kernel
 * dereferencing whatever it is handed is the hole that tier of
 * protection is supposed to close.
 */
/*
 * Where a process's own memory is, or false if it has none of its own.
 * A loaded program has the slot the loader gave it; a process the kernel
 * built has no slot, because its memory is the kernel's and is mapped in
 * every address space.
 */
bool proc_slot(const struct proc *p, uintptr_t *base, size_t *len);

/*
 * How many frames it is holding: its mapped slot pages and the tables
 * of the space it owns. A cohort member owns no space, so what its
 * cohort holds is counted against the cohort.
 */
uint32_t proc_frames(struct proc *p);

/*
 * What a page at this address in this process may be used for: text
 * read and executed, everything else written and not executed, the
 * startup record read only. One answer for the loader and for anything
 * that has to put a page back as it was.
 */
uint32_t proc_prot(const struct proc *p, uintptr_t va);

/*
 * Frames lost because a cohort place was handed on before its previous
 * holder was reaped, so the load that placed the replacement mapped
 * over the only entries that named them. Nothing can reach them and
 * nothing can free them; the count is what says how many there are.
 */
uint32_t proc_frames_orphaned(void);

/*
 * How many processes have exited and not yet been reaped. A frame count
 * taken while one is pending will see its memory come back at a moment
 * nothing chose.
 */
uint32_t proc_exited_unreaped(void);

/*
 * A handle naming this process was made, or destroyed. The second one
 * reaps the process if that was the last thing that could ask about it
 * and it has already finished.
 */
void proc_ref(struct proc *p);
void proc_unref(struct proc *p);

/*
 * A thread of this process has been reaped, so it is one fewer reason
 * the process still has to exist.
 */
void proc_thread_reaped(struct proc *p);

/* Its threads, newest first, for a caller that has to reach them all. */
struct thread *proc_threads(struct proc *p);

/*
 * How many bytes from this address the caller can reach, and whether it
 * can reach a whole range. A process the kernel
 * built reaches everything; a loaded program reaches its own image.
 *
 * This is a wider question than proc_owns: a program may name any of its
 * own memory in a descriptor the kernel reads, and may hand only memory
 * that can be mapped elsewhere to another process.
 */
size_t proc_reach_len(const struct proc *p, uintptr_t va);
bool proc_reaches(const struct proc *p, uintptr_t va, size_t len);

bool proc_owns(const struct proc *p, uintptr_t va, size_t len);

#if SYS_TIER == 2
/* The address space this process's threads run in. */
struct addrspace *proc_as(struct proc *p);
#endif
struct timeline *proc_completion(struct proc *p);

/*
 * A thread of this process has exited. The last one out advances the
 * process completion timeline and records the status.
 */
void proc_thread_exited(struct proc *p, int32_t code);

#endif /* _SYS_PROC_H_ */
