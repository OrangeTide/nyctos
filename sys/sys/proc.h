/*
 * SPDX-License-Identifier: 0BSD OR Apache-2.0
 * Copyright (C) 2026 Jon Mayo <jon@rm-f.net>
 */

/*
 * Processes.
 *
 * Section 5 makes a process an address space, a handle table, and one or
 * more threads. Only the handle table exists here: there is no address
 * space to speak of at Tier 0, and the thread list arrives with spawn.
 *
 * A process exists at all at this stage because a handle table has to
 * belong to something. Authority is per-process, not per-thread, so the
 * question "what may this code reach" is answered by the process the
 * running thread belongs to and never by the thread itself.
 */

#ifndef _SYS_PROC_H_
#define _SYS_PROC_H_

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
     * The startup record of section 20.4, and the table it points at.
     * Both live in the process rather than in the child's memory because
     * Tier 0 has no address space to place them in; section 15.2 is where
     * that changes and the loader starts writing them into the image.
     */
    sys_startup_t       p_startup;
    sys_handle_t        p_ns[SYS_NS_WORDS];

    /*
     * Where each of those handles is mounted, per section 20.4. Copied
     * for the same reason the command line is: the array a caller builds
     * is a stack frame in a program, and the child reads it after that
     * frame is gone. An entry that is only a role has an empty slot.
     */
    char                p_ns_names[SYS_NS_NAME_BYTES];

    /*
     * The command line, copied. Section 20.4 has the loader place the
     * strings in the child's memory; the kernel is the loader here, so
     * they live in the process object beside the record that points at
     * them. Copied rather than pointed at for the reason every other
     * copy here exists: the array a caller passed is a stack frame in
     * a program, and the child reads it after that frame is gone.
     */
    char                p_args[SYS_ARG_BYTES];
    uintptr_t           p_argv[SYS_MAX_ARGS + 1];

#if SYS_TIER == 2
    /*
     * This process's address space, and where its startup record
     * landed in it. Tier 0 has neither: there is one address space
     * and the record lives in this object, which a program can read
     * because nothing stops it.
     */
    struct addrspace    p_as;
#endif
    uintptr_t           p_startup_va;

    /*
     * The completion timeline of section 10.4, embedded rather than
     * drawn from the pool, and the status a joiner reads once it has
     * advanced. A process finishes when its last thread does, and the
     * code it reports is that thread's: with one thread per process
     * that is the program's exit status, and section 10.4 says nothing
     * about which thread speaks for a process with several.
     */
    struct timeline     p_completion;
    uint32_t            p_nthreads;


    /*
     * What ended the thread whose exit finished this process, if the
     * hardware did, per section 16.2. A supervisor holding a process
     * handle reads the fault here rather than having to have held a
     * thread handle it never saw.
     */
    uint32_t            p_fault;
    uintptr_t           p_fault_addr;
    uintptr_t           p_fault_pc;


    /*
     * The pager this process faults to, per section 15.3, and what it
     * has cost. The connection is resolved at spawn from the handle the
     * namespace carried, because the kernel is what sends the fault and
     * a handle means something only in the table that holds it.
     *
     * NULL is the ordinary case and means a fault is fatal: there is
     * nobody to ask, which is what section 16.2 assumes about every
     * process that is not on a paging path.
     */
    struct connection  *p_pager;
    uint64_t            p_faults;

    /*
     * Memory-locked, per section 15.4 rule 2. Everything on a paging
     * path is locked, and rule 3 makes a fault in one fatal rather than
     * recoverable: it is a configuration mistake, and a system that
     * deadlocks in the field is worse than one that stops at
     * integration.
     */
    bool                p_locked;

    /* Where a user program starts, and how it finished. */
    uintptr_t           p_entry;
    int32_t             p_exit_code;
    bool                p_exited;

    /*
     * Whether anything took this process's completion timeline. An exit
     * nobody asked about halts the system rather than passing unnoticed,
     * which is what stands in for supervision until section 24.3 has a
     * parent to make the decision properly.
     */
    bool                p_watched;
};

/*
 * Build the process the kernel's own threads belong to. Section 26 has the
 * root process holding every capability at boot and handing subsets to its
 * children; this is the degenerate case of that, with no children yet.
 */
struct proc *proc_bootstrap(const char *name);

/* The process the running thread belongs to. */
struct proc *proc_current(void);

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
 * The completion timeline of section 10.4, and the record that someone
 * is watching. Taking the timeline is what marks the process watched:
 * an exit nobody asked about still halts the machine, but the question
 * is now answered by whether anyone holds the thing that reports it
 * rather than by a flag the kernel sets on its own behalf.
 *
 * It cannot fail. The timeline is part of the process, per section 8.
 */
/*
 * Spawn on behalf of a program. Everything that can fail happens before
 * anything that cannot be undone, so a caller that gets an error back
 * has a table and a set of pools exactly as they were.
 */
/*
 * Add a thread to a process, per section 10.2. The stack is the
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
 * Whether a range of addresses is memory this process owns, per section
 * 23.2. A buffer descriptor a program passes is checked against this
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
 * How many bytes from this address the caller can reach, and whether it
 * can reach a whole range, per section 23.2 step 2. A process the kernel
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
 * process completion timeline and records the status, per section 10.4.
 */
void proc_thread_exited(struct proc *p, int32_t code);

#endif /* _SYS_PROC_H_ */
