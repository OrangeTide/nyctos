/*
 * SPDX-License-Identifier: 0BSD OR Apache-2.0
 * Copyright (C) 2026 Jon Mayo <jon@rm-f.net>
 */

#include <sys/measure.h>
#include <sys/bootarch.h>
#include <sys/channel.h>
#include <sys/cohort.h>
#include <sys/connection.h>
#include <sys/page.h>
#include <sys/handle.h>
#include <sys/object.h>
#include <sys/sched.h>
#include <sys/proc.h>
#include <sys/systm.h>
#include <sys/thread.h>
#include <sys/timeline.h>

#include <stdint.h>

/* Resolve and validate a pager, per section 15.3 and 15.4 rule 1. */
static sys_err_t proc_set_pager(struct proc *p, sys_handle_t h);

/* Copy the name a namespace entry was mounted at, per section 20.4. */
static sys_err_t proc_copy_ns_name(char *dst, const char *name);
static sys_err_t pager_chain_check(struct proc *server);

/* Copy a command line into the process that will run it. */
static sys_err_t proc_set_args(struct proc *p, const struct proc *from,
  const char *const *argv, uint32_t argc);

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

static struct proc *kernel_proc;

struct proc *
proc_kernel(void)
{
    return kernel_proc;
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
    timeline_embed(&p->p_completion, SYS_OBJ_PROCESS, p);
    p->p_nthreads = 0;

    /*
     * The first one built is the kernel's own, which every core needs
     * to reach: a core other than the first bootstraps a context of
     * its own to dispatch away from, and that context belongs here.
     */
    if (kernel_proc == NULL) {
        kernel_proc = p;
    }
    handle_table_init(&p->p_handles, p);
    p->p_cohort = NULL;

    /*
     * Section 15.4 rule 1: the chain ends here. The kernel's own
     * process has no pager and never faults recoverably, which is
     * what a terminator is, and rule 2 has everything on a paging
     * path locked.
     */
    p->p_locked = true;

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
    p->p_cohort = NULL;
    p->p_entry = 0;
    timeline_init(&p->p_completion, p->p_name);
    timeline_embed(&p->p_completion, SYS_OBJ_PROCESS, p);
    p->p_nthreads = 0;
    p->p_exit_code = 0;
    p->p_exited = false;
    p->p_watched = false;
    handle_table_init(&p->p_handles, p);

    memset(&p->p_startup, 0, sizeof(p->p_startup));
    memset(p->p_ns, 0, sizeof(p->p_ns));
    p->p_startup.version = SYS_STARTUP_VERSION;
    p->p_startup.ns_words = SYS_NS_WORDS;
    p->p_startup.ns = (uintptr_t)p->p_ns;
    p->p_startup.ns_names = (uintptr_t)p->p_ns_names;
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
        if (proc_copy_ns_name(&p->p_ns_names[i * SYS_NS_NAME_MAX],
          ns[i].name) != SYS_OK) {
            panic("spawn %s: entry %u has no name it can keep",
              name, i);
        }

        /*
         * A pager is a handle like any other and is also the one the
         * kernel has to keep hold of, per section 15.3. Resolved after
         * the move, because it is resolved in the child's table.
         */
        if (ns[i].id == SYS_NS_PAGER &&
          proc_set_pager(p, p->p_ns[i * 2 + 1]) != SYS_OK) {
            panic("spawn %s: its pager does not terminate", name);
        }
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

#if SYS_TIER == 2

/*
 * Copy the startup record, the namespace table and the command line into
 * a page of the child's own memory, and fix the pointers to be addresses
 * the child can use.
 *
 * The record is built in the process object first, because that is where
 * spawn assembles it and because Tier 0 leaves it there. What this does is
 * move it somewhere the child can reach, which at Tier 0 is unnecessary
 * and at Tier 2 is the difference between a program that starts and one
 * that faults on its first instruction reading a pointer it was handed.
 */
static void
proc_place_startup(struct proc *p, uintptr_t page_pa, uintptr_t page_va)
{
    uint8_t *dst = (uint8_t *)page_pa;
    sys_startup_t *rec = (sys_startup_t *)dst;
    size_t off = sizeof(*rec);
    sys_handle_t *ns;
    uintptr_t *argv;
    char *args;
    uint32_t i;

    *rec = p->p_startup;

    ns = (sys_handle_t *)(dst + off);
    memcpy(ns, p->p_ns, sizeof(p->p_ns));
    rec->ns = page_va + off;
    off += sizeof(p->p_ns);

    /*
     * The names beside the table, one fixed slot per pair. Fixed
     * slots are why this is a copy and an address rather than a
     * rewrite per entry: nothing inside the block points at
     * anything, so where it lands does not change what it says.
     */
    memcpy(dst + off, p->p_ns_names, sizeof(p->p_ns_names));
    rec->ns_names = page_va + off;
    off += sizeof(p->p_ns_names);

    argv = (uintptr_t *)(dst + off);
    args = (char *)(dst + off + sizeof(p->p_argv));

    memcpy(args, p->p_args, sizeof(p->p_args));

    /*
     * Each argument pointer is rewritten from where the kernel put the
     * string to where the child will find it. They are offsets into the
     * same copied block, so the arithmetic is one subtraction and one
     * addition rather than a second pass over the strings.
     */
    for (i = 0; i < SYS_MAX_ARGS + 1; i++) {
        if (p->p_argv[i] == 0) {
            argv[i] = 0;
            continue;
        }
        argv[i] = page_va + off + sizeof(p->p_argv) +
          (p->p_argv[i] - (uintptr_t)p->p_args);
    }
    rec->argv = page_va + off;

    p->p_startup_va = page_va;
}

#endif /* SYS_TIER == 2 */

#if SYS_TIER == 2

/*
 * The address space this process's threads will run in.
 *
 * A process in no cohort gets one of its own, which is what every
 * process got before section 18 was built. A member gets its cohort's,
 * built when the first member is placed and shared by every one after
 * it. Nothing else in the loader has to know which happened, because
 * proc_as() answers the same question afterwards either way.
 */
static sys_err_t
proc_space_create(struct proc *p)
{
    /*
     * A member has nothing to build. Its cohort's space was made when
     * the first member took its place, which happened before this,
     * because taking a place is what can be refused and nothing that
     * can be refused should run after a frame has been spent.
     */
    if (p->p_cohort != NULL) {
        return SYS_OK;
    }

    return vm_as_create(&p->p_as);
}

/*
 * Load a program into the address space it will run in.
 *
 * At Tier 0 an image is copied to the address it was linked for and runs
 * there, which is why one image can have only one live process. Here the
 * address it was linked for is a virtual one, the memory behind it is
 * frames nobody else has, and two copies of one image are two processes
 * that cannot see each other. That is the first thing this tier buys that
 * the other cannot have at any price.
 *
 * A cohort member is the exception to the sentence above, and the only
 * one: its slot is mapped into a space its peers are already running
 * in, at the address its own image was linked for. Slots are disjoint
 * by construction, so a second member lands beside the first rather
 * than on it, and cohort_report() stops the boot if a build ever
 * breaks that.
 *
 * The whole slot is allocated and mapped, which is a megabyte per process
 * where the programs here need a few tens of kilobytes. What it costs is
 * memory and what it buys is not having to know how much of the slot a
 * program uses: the archive carries the size of the image and not the size
 * of the memory it needs, and the difference is its zeroed data and its
 * stack. A size in the archive would fix it, and so would the demand
 * paging of stage 8, which is where the frames stop being spent in
 * advance.
 */
static sys_err_t
proc_load(struct proc *p, const struct boot_member *m)
{
    const uint8_t *src = boot_payload(m);
    uintptr_t va = (uintptr_t)m->bm_load;
    uintptr_t last = 0;
    size_t done = 0;
    sys_err_t err;

    while (done < SYS_USER_SLOT_BYTES) {
        uintptr_t pa;
        size_t n = m->bm_size > done ? m->bm_size - done : 0;

        /*
         * A page of the image, in a process that has a pager, is left
         * for the pager to supply. Section 15.1: what demand paging is
         * for is read-only text, and what is inside the image is text
         * and rodata; everything writable a program has is beyond the
         * end of the image, which is its zeroed data and its stack, and
         * that is mapped here as it always was.
         *
         * A program with initialised writable data would break this,
         * because the loader cannot see where its text ends: the archive
         * carries a size and not a layout. Section 15.1 says writable
         * segments are copied eagerly, and teaching the archive to carry
         * the boundary is what would let this obey it.
         */
        if (n > 0 && p->p_pager != NULL) {
            done += PAGE_SIZE;
            continue;
        }

        pa = page_alloc();
        if (pa == 0) {
            return SYS_ERR_NO_MEMORY;
        }
        if (n > PAGE_SIZE) {
            n = PAGE_SIZE;
        }
        if (n > 0) {
            memcpy((void *)pa, src + done, n);
        }

        /*
         * Writable and executable, because a program's text, its data and
         * its stack are one slot and nothing here knows where one ends.
         * The linker script does, and teaching the archive to carry it is
         * what would let this be three mappings instead of one.
         */
        err = md_map(proc_as(p), va + done, pa, PAGE_SIZE,
          VM_PROGRAM);
        if (err != SYS_OK) {
            return err;
        }

        last = pa;
        done += PAGE_SIZE;
    }

    p->p_entry = (uintptr_t)m->bm_load;

    /*
     * The startup record goes in the last page of the slot, which is the
     * one place in a program's memory that its own layout does not reach:
     * the stack grows down from below it and the image ends a long way
     * under that. Section 20.4 has the loader place the record in the
     * child's memory, and this is a loader.
     */
    proc_place_startup(p, last, va + SYS_USER_SLOT_BYTES - PAGE_SIZE);

    /*
     * A second member was loaded into a space its peers are running in,
     * so the addresses just filled were absent from that space a moment
     * ago. Both ports may have walked them speculatively and cached the
     * absence, and neither architecture promises it did not. Every other
     * mapping in this loader goes into a space nothing has entered yet,
     * which is why this is the only flush here.
     *
     * Removing it does not break a check on either port, which was
     * tried. It is here because the architecture manuals permit the
     * cached absence and not because anything observed one, and that
     * is the honest reason to keep it: an emulator declining to
     * speculate is not a promise about a part.
     */
    if (p->p_cohort != NULL && p->p_cohort->c_placed > 1) {
        md_tlb_flush_all();
    }

    return SYS_OK;
}

#endif /* SYS_TIER == 2 */

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
    intr_region_end((uintptr_t)__builtin_return_address(0));
    enter_user(p->p_entry, (void *)p->p_startup_va, 0);
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
  const sys_ns_spec_t *ns, uint32_t nns, const char *const *argv,
  uint32_t argc)
{
    const struct boot_member *m = boot_lookup(image);
    struct proc *p;
    struct thread *t;

    if (m == NULL) {
        panic("spawn %s: no such member in the boot archive", image);
    }
    p = proc_create(image, ns, nns);

    /* Section 18.1, and see proc_spawn_op for why it is read here. */
    p->p_cohort = cohort_of_image(image);
    if (p->p_cohort != NULL &&
      cohort_place(p->p_cohort, image, p) != SYS_OK) {
        panic("spawn %s: its place in cohort %s is taken", image,
          cohort_name(p->p_cohort));
    }

    /*
     * The command line, if there is one. The kernel spawning at boot
     * is the loader of section 20.4 as much as the operation a
     * program calls is, and a program started here that wants
     * arguments has nowhere else to get them.
     */
    if (argc > 0 && proc_set_args(p, NULL, argv, argc) != SYS_OK) {
        panic("spawn %s: it cannot have that command line", image);
    }

#if SYS_TIER == 2
    if (proc_space_create(p) != SYS_OK) {
        panic("spawn %s: no address space", image);
    }
    if (proc_load(p, m) != SYS_OK) {
        panic("spawn %s: could not load it into its own memory", image);
    }
#else
    p->p_entry = boot_load(m);
    p->p_startup_va = (uintptr_t)&p->p_startup;
#endif

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

/*
 * Where a process's own memory is, or false if it has none of its own.
 *
 * A loaded program has a slot the loader gave it. A process the kernel
 * built has no slot: its memory is the kernel's, mapped in every address
 * space, and questions about which process owns it have no answer and do
 * not need one.
 *
 * The slot is coarser than the truth, because a slot is a megabyte and a
 * program uses tens of kilobytes of it. It is the boundary the loader
 * drew and the one Tier 2 enforces with hardware, which makes it the
 * boundary worth asking about.
 */
bool
proc_slot(const struct proc *p, uintptr_t *base, size_t *len)
{
    uintptr_t wbase;
    size_t wlen;

    if (p == NULL || p->p_entry == 0) {
        return false;
    }

    boot_user_window(&wbase, &wlen);
    if (p->p_entry < wbase || p->p_entry - wbase >= wlen) {
        return false;
    }

    *base = p->p_entry;
    *len = SYS_USER_SLOT_BYTES;

    return true;
}

/*
 * How many bytes from this address the process can reach, or zero if it
 * cannot reach the address at all.
 *
 * This is the question of section 23.2 step 2, and it is not the same
 * question as proc_owns below. A program may name any of its own memory
 * in a descriptor; what it may hand to another process is narrower,
 * because handing memory over needs memory that can be mapped somewhere
 * else. The kernel reaches everything, since an address it names is one
 * it could have dereferenced without asking.
 *
 * It answers with a length rather than a yes so that a caller reading a
 * string can bound the walk. A command line with no terminator is the
 * case that made this necessary: the copy stopped after two hundred and
 * fifty-six bytes, which is a bound on the damage and not on the read.
 */
size_t
proc_reach_len(const struct proc *p, uintptr_t va)
{
    uintptr_t base;
    size_t slot;

    if (!proc_slot(p, &base, &slot)) {
        return SIZE_MAX;
    }
    if (va < base || va - base >= slot) {
        return 0;
    }

    return slot - (va - base);
}

/*
 * Whether a process can reach a whole range. Zero bytes is reachable
 * anywhere, because an operation that names no memory names none.
 */
bool
proc_reaches(const struct proc *p, uintptr_t va, size_t len)
{
    if (len == 0) {
        return true;
    }
    if (va == 0) {
        return false;
    }

    return proc_reach_len(p, va) >= len;
}

/*
 * Whether a range of addresses is memory this process may hand to
 * another, per section 23.2: a descriptor a program gives the kernel
 * names memory, and the kernel checks that the process naming it is the
 * one it belongs to rather than reading whatever the number points at.
 *
 * A process with no slot owns nothing here, and that is not a way of
 * saying the kernel has no memory. It is that kernel memory cannot be
 * lent: it is in every address space already, so there is nothing to
 * map, and it is in none of them reachable from user mode, so a program
 * given the address would fault on it. What the kernel would need to
 * lend a program a page of its own is the grant of section 19.2, which
 * is a handle and not an address.
 *
 * Refusing on both tiers is deliberate. Tier 0 has one address space and
 * the lend would work, and a rule that holds only where the hardware
 * happens to allow it is the kind of difference section 2.1 exists to
 * keep out of the interface.
 */
bool
proc_owns(const struct proc *p, uintptr_t va, size_t len)
{
    uintptr_t base;
    size_t slot;

    if (len == 0) {
        return true;
    }
    if (!proc_slot(p, &base, &slot)) {
        return false;
    }

    if (va < base || va - base >= slot) {
        return false;
    }

    return len <= slot - (va - base);
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
    /*
     * The thread is the caller: exit runs on the thread that is
     * exiting, which is what lets this take the fault from it without
     * being handed it through every layer between.
     */
    const struct thread *t = thread_current();

    if (p->p_nthreads == 0) {
        panic("proc %s: a thread exited from a process with none", p->p_name);
    }
    if (--p->p_nthreads > 0) {
        return;
    }

    p->p_exit_code = code;
    p->p_exited = true;
    if (t != NULL) {
        p->p_fault = t->t_fault;
        p->p_fault_addr = t->t_fault_addr;
        p->p_fault_pc = t->t_fault_pc;
    }

    /*
     * Anything blocked on a channel this process owned is released
     * now, per section 12.5. A server that has gone is not going to
     * receive or reply, and a client waiting on one is waiting for
     * something that cannot happen.
     */
    channel_owner_died(p);

    /*
     * An exit nobody was waiting for stops the system, named. This is the
     * placeholder for supervision that M2 introduced, now asking a better
     * question: not whether the kernel remembered to set a flag, but
     * whether anything holds the completion timeline that would report
     * this. Section 24.3 is where a parent makes the decision properly.
     */
    if (!p->p_watched) {
        if (p->p_fault != SYS_FAULT_NONE) {
            panic("%s faulted, %s at 0x%lx, pc 0x%lx, and nothing "
              "was waiting for it", p->p_name, fault_name(p->p_fault),
              (unsigned long)p->p_fault_addr,
              (unsigned long)p->p_fault_pc);
        }
        panic("%s exited with status %d and nothing was waiting for it",
          p->p_name, code);
    }

    if (timeline_signal(&p->p_completion, SYS_COMPLETION_EXITED) != SYS_OK) {
        panic("proc %s: completion advanced twice", p->p_name);
    }
}

/*
 * Whether a pager chain terminates, per section 15.4 rule 1.
 *
 * The chain is the processes that serve one another's faults, and it has
 * to end at one that cannot fault: memory-locked, with no pager of its
 * own. A chain that ends anywhere else is a deadlock waiting for the
 * right timing, and the classic shape is a storage driver that faults on
 * its own text and has to reach the device it is itself driving.
 *
 * Walking it is cheap, because it is a chain of processes and there are
 * only so many of those, and a walk longer than the pool is a cycle.
 */
static sys_err_t
pager_chain_check(struct proc *server)
{
    uint32_t hops;

    for (hops = 0; server != NULL && hops <= SYS_MAX_PROCESSES; hops++) {
        if (server->p_pager == NULL) {
            return server->p_locked ? SYS_OK : SYS_ERR_DEADLOCK;
        }
        server = connection_owner(server->p_pager);
    }

    return SYS_ERR_DEADLOCK;
}

/*
 * Resolve the pager a namespace named, per section 15.3.
 *
 * The handle has already moved into the child, which is where a handle
 * belongs; what the kernel keeps is the object, because the kernel is
 * what sends a fault message and it does not send it from a table.
 */
static sys_err_t
proc_set_pager(struct proc *p, sys_handle_t h)
{
    void *obj;
    sys_err_t err;

    if (handle_lookup(&p->p_handles, h, SYS_OBJ_CONNECTION, SYS_RIGHT_SEND,
      &obj) != SYS_OK) {
        return SYS_ERR_INVALID_ARG;
    }

    err = pager_chain_check(connection_owner(obj));
    if (err != SYS_OK) {
        return err;
    }

    /*
     * Held, and not merely pointed at. The kernel keeps the object
     * because it is what sends a fault message and it does not send
     * one from a table, but a connection now goes back to its pool
     * when its last handle closes: a process that closed the handle
     * it was given would leave this naming a freed slot, and the next
     * page fault would send its message through whatever took it.
     *
     * Nothing before stage four could reach that, because nothing
     * freed a connection. This is the same shape as the completion
     * timeline and the channel owner: a pointer that no generation
     * protects, made reachable by something being reclaimed.
     */
    if (p->p_pager != NULL) {
        connection_unref(p->p_pager);
    }
    connection_ref(obj);
    p->p_pager = obj;

    return SYS_OK;
}

/*
 * Copy the name a namespace entry was mounted at, per section 20.4.
 *
 * The name is inside the caller's array rather than pointed at from it,
 * so there is nothing further to follow and nothing further to check: the
 * array itself was checked before the operation ran. What is left is
 * whether the name ends inside its slot, and a name that does not is
 * refused rather than truncated, because half a mount point resolves to
 * something other than what the parent meant.
 */
static sys_err_t
proc_copy_ns_name(char *dst, const char *name)
{
    uint32_t n = 0;

    while (n < SYS_NS_NAME_MAX && name[n] != '\0') {
        n++;
    }
    if (n == SYS_NS_NAME_MAX) {
        return SYS_ERR_INVALID_ARG;
    }

    memcpy(dst, name, n + 1);

    return SYS_OK;
}

/*
 * Copy a command line into the process that will run it.
 *
 * The strings and the array both land in the process object, because the
 * child reads them after the caller's frame is gone and there is nowhere
 * else to put them until section 15.2's loader is writing images. What
 * this refuses is a line that does not fit: truncating a command line
 * silently is how a program ends up doing something close to what it was
 * asked.
 */
static sys_err_t
proc_set_args(struct proc *p, const struct proc *from,
  const char *const *argv, uint32_t argc)
{
    uint32_t used = 0;
    uint32_t i;

    if (argc > SYS_MAX_ARGS) {
        return SYS_ERR_INVALID_ARG;
    }

    for (i = 0; i < argc; i++) {
        const char *s = argv[i];
        size_t reach;
        uint32_t n = 0;

        if (s == NULL) {
            return SYS_ERR_INVALID_ARG;
        }

        /*
         * How far the caller's memory goes from here, per section 23.2.
         * A string with no terminator would otherwise be read until the
         * length bound below stopped it, which bounds the damage at two
         * hundred and fifty-six bytes rather than preventing it: those
         * bytes can be past the end of everything the caller has, and
         * the kernel reading them is the kernel faulting on an address
         * a program made up.
         */
        reach = proc_reach_len(from, (uintptr_t)s);
        if (reach == 0) {
            return SYS_ERR_FAULT;
        }

        while (n < reach && s[n] != '\0') {
            n++;
            if (used + n + 1 > SYS_ARG_BYTES) {
                return SYS_ERR_INVALID_ARG;
            }
        }
        if (n == reach) {
            return SYS_ERR_FAULT;
        }

        memcpy(&p->p_args[used], s, n + 1);
        p->p_argv[i] = (uintptr_t)&p->p_args[used];
        used += n + 1;
    }

    p->p_argv[argc] = 0;
    p->p_startup.argc = (int32_t)argc;
    p->p_startup.argv = (uintptr_t)p->p_argv;

    return SYS_OK;
}

#if SYS_TIER == 0

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

#endif /* SYS_TIER == 0 */

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
  const sys_ns_spec_t *ns, uint32_t nns, const char *const *argv,
  uint32_t argc, sys_handle_t *out_handle)
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
     * Two more addresses a program chose, per section 23.2: the namespace
     * array and the argument vector both live in the caller's memory and
     * are read here. The descriptor check above them says nothing about
     * either, because a pointer inside a structure is not the structure.
     */
    if (!proc_reaches(parent, (uintptr_t)ns, nns * sizeof(*ns)) ||
      !proc_reaches(parent, (uintptr_t)argv, argc * sizeof(*argv))) {
        return SYS_ERR_FAULT;
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
     * One live process per image at Tier 0, because there is one address
     * for it. Loading copies the member to the address it was linked for,
     * so a second copy of a program that is still running writes over its
     * text and its data with no way to notice. The failure that made this
     * worth refusing is that it looks fine: two short programs that never
     * overlap in time both finish normally.
     *
     * Tier 2 is where the restriction goes rather than where the check
     * gets better. The address a program runs at is virtual there and the
     * memory behind it is frames nobody else has, so two copies of one
     * image are two processes that cannot reach each other. Section 15.2
     * said a loader would end this, and an address space per process is
     * what a loader needed.
     *
     * Section 18 brings it back for cohort members, which share a space
     * and therefore share the address. cohort_place() is where that half
     * lives, and it returns this same error for this same reason. A
     * member is the only thing at Tier 2 that this rule still applies
     * to, and at Tier 0 the line below already covers it.
     */
#if SYS_TIER == 0
    if (image_in_use(m->bm_load)) {
        return SYS_ERR_WOULDBLOCK;
    }
#endif

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

        /*
         * A pager is checked here rather than after the move, because
         * section 15.4 rule 1 is a reason to refuse the spawn and a
         * refusal has to happen while there is nothing to undo.
         */
        if (spec[i].id == SYS_NS_PAGER) {
            void *cn;

            if (handle_lookup(&parent->p_handles, spec[i].handle,
              SYS_OBJ_CONNECTION, SYS_RIGHT_SEND, &cn) != SYS_OK) {
                return SYS_ERR_INVALID_ARG;
            }
            err = pager_chain_check(connection_owner(cn));
            if (err != SYS_OK) {
                return err;
            }
        }

        /*
         * The name needs no copy of its own. It is inside the entry
         * rather than pointed at from it, and the array of entries was
         * copied above for exactly that reason, so all that is left is
         * whether this one ends inside its slot. Checked before anything
         * moves, like everything else here.
         */
        if (spec[i].name[SYS_NS_NAME_MAX - 1] != '\0') {
            return SYS_ERR_INVALID_ARG;
        }
    }

    p = pool_alloc(proc_pool(), NULL);
    if (p == NULL) {
        return SYS_ERR_NO_MEMORY;
    }
    proc_init(p, image);

    /*
     * Section 18.1: which domain this image runs in is a property of
     * the build, so it is read here and not asked for. A program that
     * spawns a member gets a member, whether or not it knows that is
     * what the image is, because membership is not the spawner's to
     * decide.
     */
    p->p_cohort = cohort_of_image(image);
    if (p->p_cohort != NULL) {
        /*
         * Taken before anything is spent, because this is the one
         * refusal here that is about the request rather than about
         * the machine, and a caller told no should get a system in
         * the state it was in.
         */
        err = cohort_place(p->p_cohort, image, p);
        if (err != SYS_OK) {
            pool_free(proc_pool(), p->p_id);
            return err;
        }
    }

    t = thread_alloc(p->p_name, prio, proc_user_start, &p->p_startup);
    if (t == NULL) {
        if (p->p_cohort != NULL) {
            cohort_release(p->p_cohort, p);
        }
        pool_free(proc_pool(), p->p_id);
        return SYS_ERR_NO_MEMORY;
    }

    /*
     * The parent's handle before the handover, because it is the last
     * thing that can fail and unwinding a moved handle would mean putting
     * authority back where it came from with no way to say whether that
     * is where it started.
     */
    /*
     * With the right to end it. A parent that started a process is
     * what section 24.3 calls a supervisor, and section 10.5 says a
     * supervisor holds a handle to the process and none at all to the
     * threads inside it, so the process handle is the only thing it
     * could use. Without this a parent could watch a child wedge and
     * do nothing about it, and section 7.2 would not let it add the
     * right afterwards: duplication only attenuates.
     */
    err = handle_insert(&parent->p_handles, p, SYS_OBJ_PROCESS,
      SYS_RIGHT_WAIT | SYS_RIGHT_STAT | SYS_RIGHT_WRITE, &h);
    if (err != SYS_OK) {
        thread_set_proc(t, NULL);
        pool_free(thread_pool(), t->t_id);
        if (p->p_cohort != NULL) {
            cohort_release(p->p_cohort, p);
        }
        pool_free(proc_pool(), p->p_id);
        return err;
    }

    for (i = 0; i < nns; i++) {
        if (handle_move(&parent->p_handles, &p->p_handles, spec[i].handle,
          &p->p_ns[i * 2 + 1]) != SYS_OK) {
            panic("spawn %s: handover failed after it was checked", image);
        }
        p->p_ns[i * 2] = (sys_handle_t)spec[i].id;
        (void)proc_copy_ns_name(&p->p_ns_names[i * SYS_NS_NAME_MAX],
          spec[i].name);

        /*
         * The chain was checked before anything moved, so what is
         * left here is resolving the connection in the table it now
         * lives in. Section 15.3 has the kernel hold the object,
         * because the kernel is what sends a fault message.
         */
        if (spec[i].id == SYS_NS_PAGER) {
            (void)proc_set_pager(p, p->p_ns[i * 2 + 1]);
        }
    }

    /*
     * The command line before the image, because a line that does not
     * fit is the last thing that can fail and there is nothing after
     * it that could be undone.
     */
    err = proc_set_args(p, parent, argv, argc);
    if (err != SYS_OK) {
        handle_close(&parent->p_handles, h);
        thread_set_proc(t, NULL);
        pool_free(thread_pool(), t->t_id);
        if (p->p_cohort != NULL) {
            cohort_release(p->p_cohort, p);
        }
        pool_free(proc_pool(), p->p_id);
        return err;
    }

#if SYS_TIER == 2
    /*
     * An operation cannot panic on a failure a program can provoke.
     * Both of these run out of frames rather than out of anything the
     * caller controls, but the caller is a program and section 8 has
     * exhaustion be an error rather than a stop.
     */
    err = proc_space_create(p);
    if (err == SYS_OK) {
        err = proc_load(p, m);
    }
    if (err != SYS_OK) {
        handle_close(&parent->p_handles, h);
        thread_set_proc(t, NULL);
        pool_free(thread_pool(), t->t_id);
        if (p->p_cohort != NULL) {
            cohort_release(p->p_cohort, p);
        }
        pool_free(proc_pool(), p->p_id);
        return err;
    }
#else
    p->p_entry = boot_load(m);
    p->p_startup_va = (uintptr_t)&p->p_startup;
#endif
    thread_set_proc(t, p);
    sched_ready(t);

    if (out_handle != NULL) {
        *out_handle = h;
    }

    return SYS_OK;
}

/*
 * The first thing a thread the program created runs, in kernel mode, on
 * its own kernel stack. It drops to user mode at the entry the creator
 * named, on the stack the creator supplied, and does not come back.
 *
 * The three values travel in the thread object rather than as an
 * argument, and the thread reads them off itself rather than being
 * handed them: md_thread_init carries one word to a trampoline, this
 * needs three, and the thread is where they belong anyway because they
 * are what it is.
 */
static void
proc_thread_start(void *arg)
{
    struct thread *t = thread_current();

    (void)arg;
    intr_region_end((uintptr_t)__builtin_return_address(0));
    enter_user(t->t_uentry, (void *)t->t_uarg, t->t_ustack);
}

/*
 * Add a thread to a process, per section 10.2.
 *
 * The stack is the caller's, at any address it likes, because the kernel
 * allocates no user memory at any tier. What it checks is alignment, since
 * a misaligned stack pointer faults on one of these ports and silently
 * corrupts on the other, and neither is a good way to find out.
 */
sys_err_t
proc_thread_create(struct proc *p, uintptr_t entry, uintptr_t uarg,
  uintptr_t stack, uint8_t prio, sys_handle_t *out_handle)
{
    struct thread *t;
    sys_handle_t h;
    sys_err_t err;

    if (entry == 0 || stack == 0 || (stack & 15) != 0) {
        return SYS_ERR_INVALID_ARG;
    }
    if (prio > SYS_PRIO_MAX) {
        return SYS_ERR_INVALID_ARG;
    }
    if (prio > thread_current()->t_base_prio) {
        return SYS_ERR_PERM_DENIED;
    }

    t = thread_alloc(p->p_name, prio, proc_thread_start, NULL);
    if (t == NULL) {
        return SYS_ERR_NO_MEMORY;
    }
    t->t_uentry = entry;
    t->t_uarg = uarg;
    t->t_ustack = stack;

    err = handle_insert(&p->p_handles, t, SYS_OBJ_THREAD,
      SYS_RIGHT_WAIT | SYS_RIGHT_STAT, &h);
    if (err != SYS_OK) {
        thread_set_proc(t, NULL);
        pool_free(thread_pool(), t->t_id);
        return err;
    }

    sched_ready(t);

    if (out_handle != NULL) {
        *out_handle = h;
    }

    return SYS_OK;
}

#if SYS_TIER == 2

/*
 * The address space a thread of this process runs in.
 *
 * A process built by the loader has one of its own. The kernel's own
 * process does not: its threads run in the kernel's space, which is the
 * one every other space is a copy of.
 */
struct addrspace *
proc_as(struct proc *p)
{
    if (p == NULL) {
        return vm_kernel();
    }

    /*
     * A member runs in its cohort's space and has none of its own,
     * which is the whole of what a shared protection domain is here.
     * Asked before the process's own space, because the process's
     * own space is the thing a member does not have.
     */
    if (p->p_cohort != NULL) {
        return &p->p_cohort->c_as;
    }

    if (p->p_as.as_root == 0) {
        return vm_kernel();
    }

    return &p->p_as;
}

#endif /* SYS_TIER == 2 */

/*
 * Reaping a process, per section 10.4 read the other way round.
 *
 * A status stays readable through a handle after the object has ended,
 * so a process that has ended is not yet a process nobody can ask
 * about. It becomes one when the last handle goes and the last of its
 * threads has been reaped, and that is when the slot goes back.
 *
 * The order below is the order teardown has to have. Everything this
 * process is holding is let go before the process itself is, because
 * letting go is what can reach other objects: closing its handles drops
 * signal rights, unmaps grants, and drops references to other
 * processes, any of which can reap something else.
 */
static void
proc_reap(struct proc *p)
{
    if (!p->p_exited || p->p_refs != 0 || p->p_nthreads != 0) {
        panic("proc %s: reaped while something still had it", p->p_name);
    }

    /*
     * Its place in its cohort, before anything else can spawn into it.
     * M6 left this pointer for whatever made process slots reusable,
     * which is this.
     */
    if (channel_owned_by(p)) {
        panic("proc %s: reaped while a channel still says it owns one",
          p->p_name);
    }

    if (p->p_cohort != NULL) {
        cohort_release(p->p_cohort, p);
    }

    /*
     * Before the handles, because it is a reference of its own and
     * closing the handle it came from is not what releases it.
     */
    if (p->p_pager != NULL) {
        connection_unref(p->p_pager);
        p->p_pager = NULL;
    }

    handle_table_close_all(&p->p_handles);

    if (!pool_free(proc_pool(), p->p_id)) {
        panic("proc: the pool refused a slot it handed out");
    }
}

static void
proc_reap_if_done(struct proc *p)
{
    if (p->p_exited && p->p_refs == 0 && p->p_nthreads == 0) {
        proc_reap(p);
    }
}

void
proc_ref(struct proc *p)
{
    p->p_refs++;
}

void
proc_unref(struct proc *p)
{
    if (p->p_refs == 0) {
        panic("proc %s: unreferenced more often than referenced",
          p->p_name);
    }
    p->p_refs--;
    proc_reap_if_done(p);
}

void
proc_thread_reaped(struct proc *p)
{
    if (p->p_refs == 0) {
        panic("proc %s: a thread outlived every reference to it",
          p->p_name);
    }
    p->p_refs--;
    proc_reap_if_done(p);
}

struct thread *
proc_threads(struct proc *p)
{
    return p->p_threads;
}

sys_err_t
proc_kill(struct proc *p, int32_t code)
{
    struct thread *t;

    if (p->p_exited || p->p_nthreads == 0) {
        return SYS_ERR_WOULDBLOCK;
    }

    /*
     * The mark first, so that a thread released below and running
     * before this returns finds it. Section 10.5 makes the unblocking
     * prompt and this is what makes the leaving certain: without the
     * mark a cancelled thread would come back with an error and carry
     * on with whatever it was doing.
     */
    p->p_dying = true;
    p->p_kill_code = code;

    /*
     * Deferred, because unblocking several threads is several steps
     * and any of them can run at once if it outranks this one. The
     * list being walked is the process's own, and a thread that ran
     * and exited would be taken out of it mid-walk.
     */
    sched_defer();

    for (t = p->p_threads; t != NULL; t = t->t_sibling) {
        /*
         * Exactly as if each had been cancelled, which is the
         * section's phrase and also the implementation: a thread that
         * is not blocked has nothing to be released from and will find
         * the mark on its own.
         */
        (void)thread_cancel(t);
    }

    sched_undefer();

    return SYS_OK;
}
