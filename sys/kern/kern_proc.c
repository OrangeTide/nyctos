/*
 * SPDX-License-Identifier: 0BSD OR Apache-2.0
 * Copyright (C) 2026 Jon Mayo <jon@rm-f.net>
 */

#include <sys/handle.h>
#include <sys/object.h>
#include <sys/sched.h>
#include <sys/proc.h>
#include <sys/systm.h>
#include <sys/thread.h>

struct proc *
proc_bootstrap(const char *name)
{
    struct proc *p;

    p = pool_alloc(proc_pool(), NULL);
    if (p == NULL) {
        panic("proc: no slot for the first process");
    }

    p->p_id = pool_id_of(proc_pool(), p);
    p->p_name = name;
    handle_table_init(&p->p_handles);

    return p;
}

struct proc *
proc_current(void)
{
    struct thread *t = thread_current();

    if (t == NULL || t->t_proc == NULL) {
        panic("proc: no current process");
    }

    return t->t_proc;
}

struct proc *
proc_spawn(const char *name, uint8_t prio, void (*entry)(void *),
  const sys_handle_t *inherit, uint32_t ninherit)
{
    struct handle_table *parent = &proc_current()->p_handles;
    struct thread *t;
    struct proc *p;
    uint32_t i;

    if (ninherit > PROC_MAX_STARTUP_HANDLES) {
        panic("spawn %s: too many handles to inherit", name);
    }

    p = pool_alloc(proc_pool(), NULL);
    if (p == NULL) {
        panic("spawn %s: process pool exhausted", name);
    }

    p->p_id = pool_id_of(proc_pool(), p);
    p->p_name = name;
    handle_table_init(&p->p_handles);
    p->p_startup.ns_count = ninherit;

    /*
     * Moved rather than copied, per section 7.4. The parent gives up what
     * it hands over unless it deliberately duplicated first, which is what
     * makes the child's authority describable by looking at one list.
     */
    for (i = 0; i < ninherit; i++) {
        if (handle_move(parent, &p->p_handles, inherit[i],
          &p->p_startup.ns_handle[i]) != SYS_OK) {
            panic("spawn %s: could not hand over handle %u", name, i);
        }
    }

    t = thread_create(name, prio, entry, &p->p_startup);
    t->t_proc = p;

    sched_ready(t);

    return p;
}
