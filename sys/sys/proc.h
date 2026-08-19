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

struct proc {
    sys_id_t            p_id;
    const char         *p_name;
    struct handle_table p_handles;
};

/*
 * Build the process the kernel's own threads belong to. Section 26 has the
 * root process holding every capability at boot and handing subsets to its
 * children; this is the degenerate case of that, with no children yet.
 */
struct proc *proc_bootstrap(const char *name);

/* The process the running thread belongs to. */
struct proc *proc_current(void);

#endif /* _SYS_PROC_H_ */
