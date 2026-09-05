/*
 * SPDX-License-Identifier: 0BSD OR Apache-2.0
 *
 * PUBLIC DOMAIN (CC0-1.0)
 */

/*
 * Cores, and the state that belongs to one rather than to the system.
 *
 * Section 11.4 gives each core its own run queue and its own scheduling
 * decisions, so everything the scheduler reads on the way to a decision
 * has to be reachable without asking anybody else. That is what this
 * structure is: found through a register the port sets once, so finding
 * it costs no memory reference and no lock.
 *
 * Which fields are here is not a matter of taste. They are the ones that
 * answer "what is this core doing", and every one of them was a global
 * until there were two cores to disagree about it.
 *
 * Section 13.3 puts multiprocessing at Tier 2 only: Tier 0 disables
 * interrupts inside the kernel and bounds latency by where the
 * preemption points are, on the argument that such parts are simple and
 * often uniprocessor. So a Tier 0 build has one of these and never looks
 * for a second.
 */

#ifndef _SYS_CPU_H_
#define _SYS_CPU_H_

#include <sys/config.h>
#include <sys/intr.h>
#include <sys/op.h>

#include <stdbool.h>
#include <stdint.h>

struct thread;

/*
 * A FIFO per priority level, per section 11.2, and one set of them per
 * core. Section 11.4 has each core decide from its own queue and take no
 * lock another core could be holding, which is the whole of why this is
 * here rather than a static in the scheduler.
 */
struct runq {
    struct thread  *rq_head;
    struct thread  *rq_tail;
};

struct cpu {
    uint32_t        cp_index;

    /* What is running here, and the space it is running in. */
    struct thread  *cp_current;
    uintptr_t       cp_as_root;

    /*
     * The deferral of section 11: a short run of readies made atomic.
     * Per core because that is what it always meant. Its comment said
     * "one counter for the system, not one per thread", and per system
     * is per core the moment there are two.
     */
    uint32_t        cp_defer;
    bool            cp_defer_pending;

    /*
     * A dead thread waiting to be collected, per M7 stage one: its slot
     * holds the stack it was running on, so it cannot be freed until
     * something else is running. Per core because two cores switch at
     * once, and one slot for the system would lose one of them.
     */
    struct thread  *cp_reap;

    /*
     * This core's run queues and the bitmap of which are not empty.
     * Touched by this core only, with interrupts disabled, and never
     * by another: a thread another core wants to run here arrives
     * through the list below instead.
     */
    struct runq         cp_runq[SYS_PRIO_COUNT];
    uint32_t            cp_ready;

    /*
     * Threads another core has made runnable here, newest first,
     * pushed with a compare and exchange and taken off in one
     * exchange. A list and not a queue because the order threads
     * arrive in from elsewhere is not a scheduling decision: this core
     * puts each one on the queue its priority names, and that is where
     * the order comes from.
     *
     * Section 11.4 is the reason this is not simply a lock around the
     * queues. A core that had to take one to dispatch would be waiting
     * on a decision another core is making, which is exactly what a
     * per-core queue is for avoiding.
     */
    struct thread      *cp_incoming;

    /*
     * How many times this core has taken a lock another core could be
     * holding. Per core because the question is what this core's
     * dispatch path did, and a system-wide count answers what the
     * machine did.
     */
    uint64_t            cp_locks;

    /*
     * How many scheduling decisions this core has taken. Per core for
     * the same reason as the count above: the other core is taking its
     * own the whole time.
     */
    uint64_t            cp_dispatches;

    /*
     * Another core changed what is pending and this core is the one
     * whose hardware is armed for it. See kern_time.c.
     */
    uint32_t            cp_rearm;

    /*
     * Section 13.3's first term, measured on this core: when the
     * current interrupts-disabled region began and who began it, and
     * the longest one seen so far with the same. Per core because a
     * region is disabled on a core and nowhere else.
     */
    uint64_t            cp_intr_start;
    uintptr_t           cp_intr_site;
    uint64_t            cp_intr_worst;
    uintptr_t           cp_intr_worst_site;

    /*
     * And where it ended. The site a region begins at says who turned
     * interrupts off; it does not say where the time went, because
     * everything between the two is inside the region and a context
     * switch can put another thread's work there.
     */
    uintptr_t           cp_intr_worst_end;
    uint64_t            cp_intr_worst_at;

    /*
     * And the same again, ignoring the regions the kernel's own
     * console opens. Printing a line to a polled UART with interrupts
     * off is a real region and is reported as one, but it is the debug
     * facility of section 22 rather than a path a system does work
     * through, and left alone it is the longest region here: it
     * displaced the figure section 13.3 asks for the moment the
     * console started holding a message together. Both are printed,
     * because hiding either would be answering a different question
     * than the one asked.
     */
    bool                cp_intr_console;
    uint64_t            cp_intr_worst_kern;
    uintptr_t           cp_intr_worst_kern_site;
    uint64_t            cp_intr_regions_kern;
    uint64_t            cp_intr_total_kern;

    /*
     * And what the instrument did not see. A region whose end is
     * reached with none open cannot be charged for, and one that
     * begins while another is still open replaces it. Both are
     * discarded by design and neither was counted, so the reported
     * number of regions could have been a fraction of the real one
     * and the report would have looked exactly the same.
     */
    uint64_t            cp_intr_lost_end;
    uint64_t            cp_intr_lost_begin;

    /*
     * Which thread was running when this region began, so that a
     * region ending in another one can be recognised as having
     * spanned a context switch. Section 13.3's first term is about
     * exactly those, and asking the question here rather than at each
     * site means every path is covered without any of them opting in.
     */
    struct thread      *cp_intr_thread;

    /*
     * The longest region that ended in a thread other than the one it
     * began in, and where it began. Kept apart from the longest
     * region overall because the two are not always the same one, and
     * a run in which some other path was longer would otherwise say
     * nothing about the one this milestone is about.
     */
    uint64_t            cp_cross_worst;
    uintptr_t           cp_cross_site;

    /*
     * The same, over a window a check has cleared, so that asking what
     * one batch cost does not throw away what the run cost. They were
     * one pair of fields, and the run-wide figure survived only
     * because the largest region happened after the last clearing.
     */
    uint64_t            cp_win_worst;
    uintptr_t           cp_win_site;

    /*
     * And how many regions the window held. This is the countable
     * half of the same question: a stretch of code that disables
     * interrupts once for the whole of itself has one region however
     * much work is inside it, and one that lets them in between each
     * piece has a region per piece. The count says which, and says it
     * the same way on every host, which the duration does not.
     */
    uint64_t            cp_win_regions;

    /*
     * Crossings of the trap edge, and checks made on the submission
     * path. Section 17 exists to reduce the first; whether it reduces
     * the second is the question nobody had asked.
     */
    uint64_t            cp_syscalls;
    uint64_t            cp_checks;
    uint64_t            cp_intr_regions;
    uint64_t            cp_intr_total;
    bool                cp_intr_open;

    /*
     * Preemption points passed on this core, per section 13.3, and
     * whether this core is inside an interrupt handler. The second is
     * how a thread made runnable by an interrupt is told apart from
     * one made runnable by another thread.
     */
    uint64_t            cp_points;
    bool                cp_in_irq;

    /*
     * How many context switches happened with that flag set, which is
     * how often the handler let something else run while it was still
     * the handler. It is a count of a hazard rather than of work.
     */
    uint64_t            cp_irq_switches;

    /*
     * Section 13.3's second term, measured on this core: how many
     * interrupts have been dispatched all the way to a user thread,
     * the longest such dispatch, and the points it passed.
     */
    uint64_t            cp_disp_count;
    uint64_t            cp_disp_longest;
    uint64_t            cp_disp_points;

    /*
     * What this core has spent on the other one being separate: pokes
     * sent, and donations raised on a thread this core does not own.
     * Both are zero on a machine with one core by construction, so
     * they are the price of partitioning stated as a number.
     */
    uint64_t            cp_ipi;
    uint64_t            cp_donate_across;

    /*
     * How many times this core has had nothing to do, and whether it
     * has answered at all. These two are the only fields one core
     * writes and another reads, so they are read every time rather
     * than once into a register: a spin on a value the compiler is
     * allowed to cache is a spin that never ends.
     */
    /*
     * How deep this core is inside the console. A message is printed
     * by several calls, and the calls nest, so the lock is taken by
     * the outermost one and a count is what knows which that was.
     */
    uint32_t            cp_cn_depth;

    /*
     * Whether this core is already in here. A panic prints, and
     * printing dereferences whatever the caller handed it: the thread
     * name in an unhandled kernel trap is read out of a structure the
     * fault may be the first evidence of having corrupted. Faulting
     * inside the message re-enters this function, which prints, which
     * faults, and the machine that was supposed to stop spews half a
     * line forever instead. It did: twenty-five kilobytes of `panic:
     * unhandled trap in ` and no halt until the emulator was killed.
     */
    bool                cp_panicking;

    volatile uint64_t   cp_idle;
    volatile bool       cp_up;
};

/*
 * This core's state, from the register the port set. Cheap enough to
 * call anywhere, which is what lets the fields above be read on the
 * scheduling path.
 */
struct cpu *cpu_self(void);

/* By index, for a caller arranging something on another core. */
struct cpu *cpu_at(uint32_t index);

/* How many the port found, which is one unless this is Tier 2. */
uint32_t cpu_count(void);

/*
 * Prepare the array and claim the first entry for the core running this.
 * Called once, before anything else here.
 */
void cpu_init(void);

/*
 * Claim an entry for a core that has just arrived, and point its
 * register at it. Called on that core.
 */
void cpu_claim(uint32_t index);

/* Bring up every core the port can find, and wait for them to answer. */
void cpu_start_others(void);

/* What each core is doing, beside the other object reports. */
void cpu_report(void);

/*
 * What separating the cores has cost so far: pokes sent between them,
 * and donations raised on a thread the raising core does not own. Both
 * are zero on a machine with one core.
 */
uint64_t cpu_crossings(uint64_t *donations);

/* And how much of the run each of them spent with nothing to do. */
void cpu_idle_report(void);

/*
 * What a port provides.
 *
 * md_cpu_set_self and md_cpu_get_self are one register between them:
 * tp on RISC-V and TPIDR_EL1 on AArch64, both of which supervisor mode
 * owns and neither of which this kernel used for anything else.
 *
 * md_cpu_start asks the hardware to run a core that is not running.
 * How differs completely: the RISC-V harts all arrive at the reset
 * vector because there is no firmware under this kernel, so starting
 * one is releasing it from a wait, and the AArch64 secondaries are held
 * off by the platform and are started with the same PSCI interface the
 * port already uses to power the machine down.
 *
 * Where it arrives is not the caller's to choose. A core that has just
 * started has no stack, no vector table and no translation, so each port
 * sends it somewhere of its own that establishes all three, and that
 * place enters cpu_secondary_entry when it is done. This took an entry
 * point as an argument once, which one port honoured and the other
 * quietly discarded.
 */
void md_cpu_set_self(struct cpu *c);
struct cpu *md_cpu_get_self(void);
sys_err_t md_cpu_start(uint32_t index);

/* How many the hardware says it has, at most SYS_MAX_CORES. */
uint32_t md_cpu_available(void);

/* A hint that this core is spinning, and a wait for an interrupt. */
void md_cpu_relax(void);

/*
 * Everything this core wrote is visible to another core before anything
 * it writes next. Both ports order memory weakly, so the word that says
 * a core has arrived can be seen before the state that word describes,
 * and a reader would find a core that is up and has nothing in it.
 */
void md_cpu_fence(void);
void md_cpu_wait(void);

/* Where a core that has just started arrives. Defined by the kernel. */
void cpu_secondary_entry(void);

/*
 * Which core this is, asked on a core that has just arrived and has no
 * register set yet. The one thing here that cannot go through cpu_self.
 */
uint32_t md_cpu_arriving(void);

#endif /* _SYS_CPU_H_ */
