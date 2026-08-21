/*
 * SPDX-License-Identifier: 0BSD OR Apache-2.0
 * Copyright (C) 2026 Jon Mayo <jon@rm-f.net>
 */

#include <sys/bootarch.h>
#include <sys/handle.h>
#include <sys/object.h>
#include <sys/sched.h>
#include <sys/proc.h>
#include <sys/systm.h>
#include <sys/thread.h>
#include <sys/timeline.h>

/* Copy a name into the process, truncating rather than running over. */
static void
proc_set_name(struct proc *p, const char *name)
{
    uint32_t i;

    for (i = 0; i < SYS_IMAGE_NAME_MAX && name[i] != '\0'; i++) {
        p->p_name[i] = name[i];
    }
    p->p_name[i] = '\0';
}

struct proc *
proc_bootstrap(const char *name)
{
    struct proc *p;

    p = pool_alloc(proc_pool(), NULL);
    if (p == NULL) {
        panic("proc: no slot for the first process");
    }

    p->p_id = pool_id_of(proc_pool(), p);
    proc_set_name(p, name);
    timeline_init(&p->p_completion, p->p_name);
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
 * Fill in a process object that has just come out of the pool. Split
 * out because the operation of section 10.1 allocates and initialises
 * in a different order from the kernel's own spawn: it has to be able
 * to give the slot back.
 */
static void
proc_init(struct proc *p, const char *name)
{
    p->p_id = pool_id_of(proc_pool(), p);
    proc_set_name(p, name);
    p->p_entry = 0;
    timeline_init(&p->p_completion, p->p_name);
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
}

/*
 * The part of spawning that does not care whether the child is a kernel
 * thread or a loaded program: a process, a handle table, and the startup
 * record of section 20.4 filled in from the parent's handles.
 */
static struct proc *
proc_create(const char *name, const sys_ns_spec_t *ns, uint32_t nns)
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

    proc_init(p, name);

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
  const sys_ns_spec_t *ns, uint32_t nns)
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
  const sys_ns_spec_t *ns, uint32_t nns)
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

/*
 * Undertake to look at how this process finished. Taking its
 * completion timeline says so, and so does holding a handle to it, and
 * both go through here.
 */
void
proc_watch(struct proc *p)
{
    p->p_watched = true;
}

struct timeline *
proc_completion(struct proc *p)
{
    proc_watch(p);

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


/*
 * Is something still running at this address?
 *
 * There is one address per image and no MMU, so a process is where its
 * member was linked to be. Scanning the pool is sixteen comparisons and
 * needs no bookkeeping that could go stale; a process that has exited does
 * not count, because nothing of it is still executing even though its slot
 * is kept until M7 reaps it.
 */
static bool
image_in_use(uintptr_t load)
{
    struct pool *pool = proc_pool();
    uint32_t i;

    for (i = 0; i < pool->count; i++) {
        sys_id_t id = pool_id_at(pool, i);
        const struct proc *q;

        if (id == SYS_ID_INVALID) {
            continue;
        }
        q = pool_lookup(pool, id);
        if (q != NULL && q->p_entry == load && !q->p_exited) {
            return true;
        }
    }

    return false;
}
/*
 * Spawn on behalf of a program, per section 10.1 and section 20.4.
 *
 * The difference from proc_spawn_image is not what it builds but what it
 * does when it cannot. A kernel caller assembling the system has nowhere
 * to report a failure and nothing to do about one, so it panics. A caller
 * that is a program gets an error and decides for itself, and it has to
 * get that error with nothing half built behind it.
 *
 * The order below is the whole of how that is arranged. Everything that
 * can fail happens before anything that cannot be undone: the arguments,
 * the image, and every handle the caller named are checked first; then the
 * two pool allocations and the parent's handle, each of which unwinds what
 * came before it; and only then are the handles moved and the image
 * copied, neither of which can fail once the checks above have passed.
 */
sys_err_t
proc_spawn_op(struct proc *parent, const char *image, uint8_t prio,
  const sys_ns_spec_t *ns, uint32_t nns, sys_handle_t *out_handle)
{
    sys_ns_spec_t spec[SYS_NS_MAX];
    const struct boot_member *m;
    struct proc *p;
    struct thread *t;
    sys_handle_t h;
    sys_err_t err;
    uint32_t i;

    if (nns > SYS_NS_MAX || (nns > 0 && ns == NULL)) {
        return SYS_ERR_INVALID_ARG;
    }
    if (prio > SYS_PRIO_MAX) {
        return SYS_ERR_INVALID_ARG;
    }

    /*
     * A child may not outrank its parent. Nothing in section 11 says who
     * chooses a priority, and the answer that follows from the rest of the
     * design is that authority flows downward: a program that could start
     * something above itself would be granting time it was not given, and
     * with no quantum anywhere a thread that does not block owns the core
     * until it stops. The kernel spawning at boot is not this path.
     */
    if (prio > thread_current()->t_base_prio) {
        return SYS_ERR_PERM_DENIED;
    }

    /*
     * The array is copied before it is looked at, and everything below
     * reads the copy. Section 23.2 has validation happen once before
     * anything executes, and once means once: the caller's array is
     * memory a program owns, so a check against it and a use of it are
     * two reads of something that can change between them the moment a
     * process has a second thread.
     */
    for (i = 0; i < nns; i++) {
        spec[i] = ns[i];
    }

    m = boot_lookup(image);
    if (m == NULL) {
        return SYS_ERR_NO_ENT;
    }

    /*
     * One live process per image, because there is one address for it.
     * Loading copies the member to the address it was linked for, so a
     * second copy of a program that is still running writes over its
     * text and its data with no way to notice. The failure that made
     * this worth refusing is that it looks fine: two short programs
     * that never overlap in time both finish normally. Section 15.2's
     * loader is what removes the restriction rather than the check.
     */
    if (image_in_use(m->bm_load)) {
        return SYS_ERR_WOULDBLOCK;
    }

    /*
     * Every handle checked before any of them moves. A caller that names
     * one it does not hold, or one it may not transfer, is refused with
     * its table exactly as it was.
     */
    for (i = 0; i < nns; i++) {
        if (spec[i].id == SYS_NS_NONE) {
            return SYS_ERR_INVALID_ARG;
        }
        err = handle_lookup(&parent->p_handles, spec[i].handle,
          SYS_OBJ_NONE, SYS_RIGHT_TRANSFER, NULL);
        if (err != SYS_OK) {
            return err;
        }
    }

    p = pool_alloc(proc_pool(), NULL);
    if (p == NULL) {
        return SYS_ERR_NO_MEMORY;
    }
    proc_init(p, image);

    t = thread_alloc(p->p_name, prio, proc_user_start, &p->p_startup);
    if (t == NULL) {
        pool_free(proc_pool(), p->p_id);
        return SYS_ERR_NO_MEMORY;
    }

    /*
     * The parent's handle before the handover, because it is the last
     * thing that can fail and unwinding a moved handle would mean putting
     * authority back where it came from with no way to say whether that
     * is where it started.
     */
    err = handle_insert(&parent->p_handles, p, SYS_OBJ_PROCESS,
      SYS_RIGHT_WAIT | SYS_RIGHT_STAT, &h);
    if (err != SYS_OK) {
        thread_set_proc(t, NULL);
        pool_free(thread_pool(), t->t_id);
        pool_free(proc_pool(), p->p_id);
        return err;
    }

    for (i = 0; i < nns; i++) {
        if (handle_move(&parent->p_handles, &p->p_handles, spec[i].handle,
          &p->p_ns[i * 2 + 1]) != SYS_OK) {
            panic("spawn %s: handover failed after it was checked", image);
        }
        p->p_ns[i * 2] = (sys_handle_t)spec[i].id;
    }

    p->p_entry = boot_load(m);
    thread_set_proc(t, p);
    sched_ready(t);

    if (out_handle != NULL) {
        *out_handle = h;
    }

    return SYS_OK;
}
