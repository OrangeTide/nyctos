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

#include <sys/handle.h>
#include <sys/pool.h>
#include <sys/startup.h>
#include <sys/timeline.h>

#include <stdbool.h>

struct proc {
    sys_id_t            p_id;
    /*
     * Copied, not pointed at. The name a spawn operation carries lives
     * in the caller'''s argument structure, which is a stack frame in a
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
     * The completion timeline of section 10.4, embedded rather than
     * drawn from the pool, and the status a joiner reads once it has
     * advanced. A process finishes when its last thread does, and the
     * code it reports is that thread's: with one thread per process
     * that is the program's exit status, and section 10.4 says nothing
     * about which thread speaks for a process with several.
     */
    struct timeline     p_completion;
    uint32_t            p_nthreads;

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
  const sys_ns_spec_t *ns, uint32_t nns);

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
sys_err_t proc_spawn_op(struct proc *parent, const char *image,
  uint8_t prio, const sys_ns_spec_t *ns, uint32_t nns,
  sys_handle_t *out_handle);

void proc_watch(struct proc *p);
struct timeline *proc_completion(struct proc *p);

/*
 * A thread of this process has exited. The last one out advances the
 * process completion timeline and records the status, per section 10.4.
 */
void proc_thread_exited(struct proc *p, int32_t code);

#endif /* _SYS_PROC_H_ */
