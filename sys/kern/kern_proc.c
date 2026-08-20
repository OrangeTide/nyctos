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

/*
 * Copy a namespace entry name, refusing one that does not fit rather than
 * truncating it. A truncated name is a name for something else, and a
 * child that looks up the short form would silently reach the wrong
 * handle.
 */
static void
ns_set_name(struct proc *p, sys_ns_entry_t *e, const char *name)
{
    size_t i;

    for (i = 0; i < SYS_NS_NAME_MAX; i++) {
        e->name[i] = name[i];
        if (name[i] == '\0') {
            return;
        }
    }

    panic("spawn %s: namespace name '%s' does not fit", p->p_name, name);
}

struct proc *
proc_spawn(const char *name, uint8_t prio, void (*entry)(void *),
  const struct proc_ns_spec *ns, uint32_t nns)
{
    struct handle_table *parent = &proc_current()->p_handles;
    struct thread *t;
    struct proc *p;
    uint32_t i;

    if (nns > SYS_NS_MAX) {
        panic("spawn %s: too many handles to inherit", name);
    }

    p = pool_alloc(proc_pool(), NULL);
    if (p == NULL) {
        panic("spawn %s: process pool exhausted", name);
    }

    p->p_id = pool_id_of(proc_pool(), p);
    p->p_name = name;
    handle_table_init(&p->p_handles);

    memset(&p->p_startup, 0, sizeof(p->p_startup));
    memset(p->p_ns, 0, sizeof(p->p_ns));
    p->p_startup.version = SYS_STARTUP_VERSION;
    p->p_startup.ns_count = nns;
    p->p_startup.ns = (uintptr_t)p->p_ns;

    /*
     * Moved rather than copied, per section 7.4. The parent gives up what
     * it hands over unless it deliberately duplicated first, which is what
     * makes the child's authority describable by looking at one list.
     */
    for (i = 0; i < nns; i++) {
        ns_set_name(p, &p->p_ns[i], ns[i].name);
        p->p_ns[i].type = SYS_OBJ_NONE;
        if (handle_move(parent, &p->p_handles, ns[i].handle,
          &p->p_ns[i].handle) != SYS_OK) {
            panic("spawn %s: could not hand over handle %u", name, i);
        }
    }

    t = thread_create(name, prio, entry, &p->p_startup);
    t->t_proc = p;

    sched_ready(t);

    return p;
}
