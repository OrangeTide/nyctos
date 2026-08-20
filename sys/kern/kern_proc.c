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
#include <sys/timeline.h>

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
    timeline_init(&p->p_completion, name);
    p->p_nthreads = 0;
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
 * The part of spawning that does not care whether the child is a kernel
 * thread or a loaded program: a process, a handle table, and the startup
 * record of section 20.4 filled in from the parent's handles.
 */
static struct proc *
proc_create(const char *name, const struct proc_ns_spec *ns, uint32_t nns)
{
    struct handle_table *parent = &proc_current()->p_handles;
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
    p->p_entry = 0;
    timeline_init(&p->p_completion, name);
    p->p_nthreads = 0;
    p->p_exit_code = 0;
    p->p_exited = false;
    p->p_watched = false;
    handle_table_init(&p->p_handles);

    memset(&p->p_startup, 0, sizeof(p->p_startup));
    memset(p->p_ns, 0, sizeof(p->p_ns));
    p->p_startup.version = SYS_STARTUP_VERSION;
    p->p_startup.ns_words = SYS_NS_WORDS;
    p->p_startup.ns = (uintptr_t)p->p_ns;

    /*
     * Moved rather than copied, per section 7.4. The parent gives up what
     * it hands over unless it deliberately duplicated first, which is what
     * makes the child's authority describable by looking at one list.
     *
     * The identifier is written after the move succeeds, so a slot the
     * move failed on stays SYS_NS_NONE rather than naming a handle that is
     * not there.
     */
    for (i = 0; i < nns; i++) {
        if (ns[i].id == SYS_NS_NONE) {
            panic("spawn %s: entry %u has no identifier", name, i);
        }
        if (handle_move(parent, &p->p_handles, ns[i].handle,
          &p->p_ns[i * 2 + 1]) != SYS_OK) {
            panic("spawn %s: could not hand over handle %u", name, i);
        }
        p->p_ns[i * 2] = (sys_handle_t)ns[i].id;
    }

    return p;
}

struct proc *
proc_spawn(const char *name, uint8_t prio, void (*entry)(void *),
  const struct proc_ns_spec *ns, uint32_t nns)
{
    struct proc *p = proc_create(name, ns, nns);
    struct thread *t;

    t = thread_create(name, prio, entry, &p->p_startup);
    thread_set_proc(t, p);

    sched_ready(t);

    return p;
}

/*
 * The first thing a loaded program's thread runs, in kernel mode, on the
 * thread's own kernel stack. It drops to user mode and does not come back;
 * the stack it leaves behind is where the trap path will build its frame
 * on the next system call, which is why the trampoline must not return.
 */
static void
proc_user_start(void *arg)
{
    struct proc *p = proc_current();

    (void)arg;
    enter_user(p->p_entry, &p->p_startup);
}

/*
 * Spawn a program from the boot archive. The image is copied to the
 * address it was linked for before the thread is made runnable, so the
 * copy happens in the caller's context and a failure is the caller's to
 * see rather than a fault in a process that has already started.
 *
 * The startup record the child is given points into this process
 * structure, which is kernel memory the program can read because Tier 0
 * has no protection to stop it. That is the tier's bargain, per section
 * 6.1, and section 15.2 is where a loader places the record in the image
 * instead.
 */
struct proc *
proc_spawn_image(const char *image, uint8_t prio,
  const struct proc_ns_spec *ns, uint32_t nns)
{
    const struct boot_member *m = boot_lookup(image);
    struct proc *p;
    struct thread *t;

    if (m == NULL) {
        panic("spawn %s: no such member in the boot archive", image);
    }

    p = proc_create(image, ns, nns);
    p->p_entry = boot_load(m);

    t = thread_create(image, prio, proc_user_start, &p->p_startup);
    thread_set_proc(t, p);

    sched_ready(t);

    return p;
}

struct timeline *
proc_completion(struct proc *p)
{
    p->p_watched = true;

    return &p->p_completion;
}

/*
 * A thread of this process has exited. Section 10.4 has the process
 * completion timeline advance when the process finishes, and a process
 * finishes when its last thread does.
 *
 * The status is written before the advance, because a joiner released by
 * it can outrank the exiting thread and read the status before that thread
 * runs again.
 */
void
proc_thread_exited(struct proc *p, int32_t code)
{
    if (p->p_nthreads == 0) {
        panic("proc %s: a thread exited from a process with none", p->p_name);
    }
    if (--p->p_nthreads > 0) {
        return;
    }

    p->p_exit_code = code;
    p->p_exited = true;

    /*
     * An exit nobody was waiting for stops the system, named. This is the
     * placeholder for supervision that M2 introduced, now asking a better
     * question: not whether the kernel remembered to set a flag, but
     * whether anything holds the completion timeline that would report
     * this. Section 24.3 is where a parent makes the decision properly.
     */
    if (!p->p_watched) {
        panic("%s exited with status %d and nothing was waiting for it",
          p->p_name, code);
    }

    if (timeline_signal(&p->p_completion, SYS_COMPLETION_EXITED) != SYS_OK) {
        panic("proc %s: completion advanced twice", p->p_name);
    }
}
