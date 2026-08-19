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

/*
 * What a child is handed at launch. Section 20.4 has named directory
 * handles passed at spawn and consumed by libc the way envp is; this is
 * the degenerate form of that, an ordered list with the names still
 * implicit in the order. It is a record rather than a convention because
 * handle values mean nothing outside the table holding them, so a child
 * cannot be told its handles any other way.
 */
#define PROC_MAX_STARTUP_HANDLES  8

struct proc_startup {
    uint32_t        ns_count;
    sys_handle_t    ns_handle[PROC_MAX_STARTUP_HANDLES];
};

struct proc {
    sys_id_t            p_id;
    const char         *p_name;
    struct handle_table p_handles;
    struct proc_startup p_startup;
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
  void (*entry)(void *), const sys_handle_t *inherit, uint32_t ninherit);

#endif /* _SYS_PROC_H_ */
