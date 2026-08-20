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

#include <stdbool.h>

/*
 * What the caller of proc_spawn() says a child should be given: an
 * identifier and a handle, per entry. The handle is moved rather than
 * copied, so this is a list of things the parent is giving up.
 */
struct proc_ns_spec {
    sys_ns_id_t     id;
    sys_handle_t    handle;
};

struct proc {
    sys_id_t            p_id;
    const char         *p_name;
    struct handle_table p_handles;

    /*
     * The startup record of section 20.4, and the table it points at.
     * Both live in the process rather than in the child's memory because
     * Tier 0 has no address space to place them in; section 15.2 is where
     * that changes and the loader starts writing them into the image.
     */
    sys_startup_t       p_startup;
    sys_handle_t        p_ns[SYS_NS_WORDS];

    /* Where a user program starts, and how it finished. Section 10.4
     * makes exit a completion timeline a joiner reads; this is the part
     * of that M2 needs, which is a program being able to stop. */
    uintptr_t           p_entry;
    int32_t             p_exit_code;
    bool                p_exited;

    /*
     * Whether anything is waiting for this process to finish. An exit
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
  void (*entry)(void *), const struct proc_ns_spec *ns, uint32_t nns);

/*
 * The same, for a program in the boot archive rather than a function in
 * the kernel. The image is copied to the address it was linked for and a
 * thread is started that drops into it.
 */
struct proc *proc_spawn_image(const char *image, uint8_t prio,
  const struct proc_ns_spec *ns, uint32_t nns);

#endif /* _SYS_PROC_H_ */
