/*
 * SPDX-License-Identifier: 0BSD OR Apache-2.0
 *
 * PUBLIC DOMAIN (CC0-1.0)
 */

/*
 * Cores, per section 11.4.
 *
 * The array is static like every other pool in this kernel, and it is
 * the one pool whose size the hardware gets a say in: a build says how
 * many cores it will use and the port says how many there are, and the
 * smaller wins.
 *
 * There is deliberately nothing here that a core has to take a lock to
 * read about itself. Section 11.4's whole argument is that a scheduling
 * decision decomposes into a per-core problem, and it cannot if finding
 * out what this core is doing means agreeing with another one.
 */

#include <sys/cpu.h>
#include <sys/lock.h>
#include <sys/proc.h>
#include <sys/sched.h>
#include <sys/systm.h>
#include <sys/thread.h>
#include <sys/vm.h>

static struct cpu cpus[SYS_MAX_CORES];
static uint32_t ncores = 1;

struct cpu *
cpu_self(void)
{
    return md_cpu_get_self();
}

struct cpu *
cpu_at(uint32_t index)
{
    if (index >= SYS_MAX_CORES) {
        return NULL;
    }

    return &cpus[index];
}

uint32_t
cpu_count(void)
{
    return ncores;
}

void
cpu_claim(uint32_t index)
{
    struct cpu *c;

    /*
     * Checked here and not by the caller, because the caller is a core
     * that has just started and the number is one the hardware chose.
     * A machine with more cores than the build has slots for hands one
     * of them an index past the end of this array, and the write that
     * follows would land in whatever the linker put next.
     */
    if (index >= SYS_MAX_CORES) {
        panic("cpu %u: the hardware has more cores than this build has",
          (unsigned int)index);
    }

    c = &cpus[index];
    c->cp_index = index;

    /*
     * Said last, and after everything it describes. Another core spins
     * on this word and then reads the rest of this structure, so a
     * machine free to reorder the two would show it a core that is up
     * before it is filled in.
     */
    md_cpu_fence();
    c->cp_up = true;
    md_cpu_set_self(c);
}

void
cpu_init(void)
{
    /*
     * The core running this is index zero by definition rather than by
     * hardware identity. A port whose cores are numbered oddly maps
     * them here; nothing above cares which physical core it is on, only
     * that its own state is its own.
     */
    cpu_claim(0);

    ncores = md_cpu_available();

    /*
     * A port that answers nonsense is a broken port and not a small
     * machine. Substituting what the build asked for would be writing
     * down the answer that makes the count below agree, which is the
     * one number here worth having.
     */
    if (ncores == 0 || ncores > SYS_MAX_CORES) {
        panic("cores: the port says %u, and this build has room for %u",
          (unsigned int)ncores, (unsigned int)SYS_MAX_CORES);
    }
}

void
cpu_start_others(void)
{
    uint32_t i;

    for (i = 1; i < ncores; i++) {
        uint32_t spin;

        if (md_cpu_start(i) != SYS_OK) {
            panic("cpu %u: the port could not start it", (unsigned int)i);
        }

        /*
         * Waited for, rather than started and assumed. A core that
         * never answers is a machine that looks like it has two and
         * behaves like it has one, which is the kind of difference
         * that shows up later as a scheduling decision nobody made.
         */
        for (spin = 0; spin < 1000000 && !cpus[i].cp_up; spin++) {
            md_cpu_relax();
        }
        if (!cpus[i].cp_up) {
            panic("cpu %u: started and never answered", (unsigned int)i);
        }

        /*
         * Read after the word that announced it, so that what that
         * core wrote about itself before saying it was up is what this
         * one sees below.
         */
        md_cpu_fence();
    }
}

void
cpu_report(void)
{
    uint32_t i;

    cnprintf("cores: %u of %u\n", (unsigned int)ncores,
      (unsigned int)SYS_MAX_CORES);

    for (i = 0; i < ncores; i++) {
        const struct cpu *c = &cpus[i];

        cnprintf("    core %u: %s, %s\n", (unsigned int)i,
          c->cp_up ? "up" : "silent",
          c->cp_current != NULL ? c->cp_current->t_name : "nothing running");
    }

    /*
     * And that they disagree about what they are doing, which is what
     * per-core state is for. One core is running this; every other one
     * is in the idle loop with an empty queue, because section 11.4
     * puts nothing anywhere until somebody puts it there.
     */
    for (i = 1; i < ncores; i++) {
        if (cpus[i].cp_current != NULL) {
            panic("cpu %u: running %s, and nothing was put there",
              (unsigned int)i, cpus[i].cp_current->t_name);
        }
    }

    /*
     * And that there are as many as the build asked for. A core that
     * cannot be found is not used, which is the right thing to do and
     * the wrong thing to do quietly: the emulator and the kernel are
     * configured together here, so fewer than expected is a build
     * mistake, and a milestone about two cores that quietly ran on one
     * would pass every check it has.
     */
    if (ncores != SYS_MAX_CORES) {
        panic("cores: %u answered, and this build is for %u",
          (unsigned int)ncores, (unsigned int)SYS_MAX_CORES);
    }
}

void
cpu_idle_report(void)
{
    uint32_t i;

    /*
     * Outside the compared part of the report, because how many times a
     * core went round an idle loop is a number about the host. What is
     * checked is that it moved at all: a second core whose count never
     * rose is one that never ran.
     *
     * One is the expected answer and not a small one. Nothing is
     * routed to a core with an empty queue, so it goes round once and
     * waits for an interrupt that does not come. A large number here
     * would mean an idle core spinning, which is a core taking memory
     * bandwidth from the one doing the work.
     */
    for (i = 1; i < ncores; i++) {
        cnprintf("core %u idled %lu times\n", (unsigned int)i,
          (unsigned long)cpus[i].cp_idle);
        if (cpus[i].cp_idle == 0) {
            panic("cpu %u: never reached its idle loop", (unsigned int)i);
        }
    }
}

/*
 * Where a core that has just started arrives, on its own stack and with
 * translation not yet its own.
 *
 * It claims its entry before anything else, because everything below
 * that reads what this core is doing reads it through the register that
 * claim sets. Then it enters the address space the kernel is in, which
 * every space contains, and idles: section 11.4 gives it a run queue of
 * its own and nothing has been put in it.
 */
void
cpu_secondary_entry(void)
{
    uint32_t index = md_cpu_arriving();

    cpu_claim(index);

    /*
     * That the register points at this core's own state and not at
     * somebody else's. Everything below reads what this core is doing
     * through it, so a core holding another's entry would agree with
     * that core about which thread is running, which is the one thing
     * two cores must never do.
     */
    if (cpu_self() != cpu_at(index) || cpu_self()->cp_index != index) {
        panic("cpu %u: claimed somebody else's state",
          (unsigned int)index);
    }

#if SYS_TIER == 2
    md_mmu_enable(vm_kernel());
#endif

    lock_check_secondary();

    /*
     * Its own queues, and something to be while it has nothing to run.
     * Section 11.6 has idle be a state of the core rather than a
     * thread, and this is not a thread in that sense: it is the
     * context the core dispatches away from and comes back to.
     */
    sched_init();
    thread_bootstrap("idle", 0, proc_kernel());

    sched_secondary();
}
