/*
 * SPDX-License-Identifier: 0BSD OR Apache-2.0
 *
 * PUBLIC DOMAIN (CC0-1.0)
 */

/*
 * The kernel lock, and the check that it is one.
 *
 * The atomic operations here are the compiler's rather than the port's.
 * A port provides what only it can know, and how to exchange a word
 * with the memory system is not that: both of these architectures have
 * the instruction and the compiler knows which it is. What the ports do
 * provide is the fence and the spin hint, because those are already
 * part of how a core is started.
 */

#include <sys/cpu.h>
#include <sys/lock.h>
#include <sys/systm.h>

#if SYS_TIER == 2

/* Whether this core is the one holding it. */
static bool
spin_held(const struct spinlock *l)
{
    return l->sl_locked != 0 && l->sl_owner == cpu_self()->cp_index + 1;
}

/*
 * How many times this core has taken a lock another core could be
 * holding. Section 11.4 has a core dispatch without doing that, and a
 * claim about what a path does not do is worth a counter rather than a
 * reading of the source.
 *
 * Per core, and it was not at first. One counter for the system counts
 * the other core's locks as well as this one's, so a decision taken
 * here failed because a system call was made there, which is a check
 * that reports the thing it is watching for whenever the machine is
 * busy.
 */
uint64_t
spin_take_count(void)
{
    return cpu_self()->cp_locks;
}

void
spin_lock(struct spinlock *l)
{
    uint32_t self = cpu_self()->cp_index + 1;

    cpu_self()->cp_locks++;

    /*
     * Asked before spinning and not after. A core that already holds
     * this would wait for a word that only it can clear, and it is
     * inside the lock, so it will not be clearing anything. That is a
     * machine that stops without a word about why, and the entire
     * reason to own a lock rather than merely hold it is to be able to
     * say this instead.
     */
    if (l->sl_locked != 0 && l->sl_owner == self) {
        panic("lock %s: taken again by the core already holding it",
          l->sl_name);
    }

    while (__atomic_exchange_n(&l->sl_locked, 1u, __ATOMIC_ACQUIRE) != 0) {
        md_cpu_relax();
    }

    l->sl_owner = self;
}

void
spin_unlock(struct spinlock *l)
{
    uint32_t self = cpu_self()->cp_index + 1;

    if (l->sl_locked == 0) {
        panic("lock %s: released and not held", l->sl_name);
    }
    if (l->sl_owner != self) {
        panic("lock %s: released by core %u and held by core %u",
          l->sl_name, (unsigned int)(self - 1),
          (unsigned int)(l->sl_owner - 1));
    }

    l->sl_owner = 0;
    __atomic_store_n(&l->sl_locked, 0u, __ATOMIC_RELEASE);
}


/*
 * Named where it is declared, because the first thing it ever said
 * was "lock (null): released and not held". A lock that cannot say
 * which lock it is answers half the question it exists to answer, and
 * an initialiser that has to be called is one that can be forgotten.
 */
static struct spinlock kernel_lock = { 0, 0, "kernel" };

static bool
klock_held(void)
{
    return spin_held(&kernel_lock);
}

intr_state_t
klock_enter(void)
{
    intr_state_t s = md_intr_disable();

    spin_lock(&kernel_lock);

    return s;
}

void
klock_exit(intr_state_t s)
{
    spin_unlock(&kernel_lock);
    md_intr_restore(s);
}

bool
klock_drop(void)
{
    if (!klock_held()) {
        return false;
    }

    spin_unlock(&kernel_lock);

    return true;
}

void
klock_regain(bool had)
{
    if (had) {
        spin_lock(&kernel_lock);
    }
}

#endif /* SYS_TIER == 2 */

/*
 * What the two cores fight over.
 *
 * The invariant is deliberately one a lost update breaks rather than one
 * a torn word breaks: the count and its shadow are written from a value
 * read at the top of the region, so two cores interleaving there leave
 * a shadow that does not match the count they both wrote.
 *
 * The flag is the more direct statement of the same thing. It says a
 * core is inside, and a core that finds it set on the way in has found
 * the other one still in there.
 */
#define HAMMER_ROUNDS  4096

static struct {
    volatile uint32_t   hm_inside;
    uint64_t            hm_count;
    uint64_t            hm_shadow;
    uint32_t            hm_by[SYS_MAX_CORES];
} hammer;

static volatile uint32_t hammer_go;
static volatile uint32_t hammer_done;

static void
hammer_once(void)
{
    intr_state_t s;
    uint64_t n;
    uint32_t i;

    s = klock_enter();

    if (hammer.hm_inside != 0) {
        panic("lock check: two cores inside the region at once");
    }
    hammer.hm_inside = 1;

    /*
     * Read, then written back from what was read, with the window
     * between them widened on purpose. A region that is exclusive does
     * not care how long it is; one that is not cares a great deal, and
     * a race that needs an exact interleaving to show is a race this
     * check would report as absent.
     */
    n = hammer.hm_count;
    for (i = 0; i < 16; i++) {
        md_cpu_relax();
    }
    hammer.hm_count = n + 1;
    hammer.hm_shadow = (n + 1) * 3;
    hammer.hm_by[cpu_self()->cp_index]++;

    hammer.hm_inside = 0;

    klock_exit(s);
}

void
lock_check_secondary(void)
{
    uint32_t i;

    while (hammer_go == 0) {
        md_cpu_relax();
    }

    for (i = 0; i < HAMMER_ROUNDS; i++) {
        hammer_once();
    }

    md_cpu_fence();
    hammer_done = 1;
}

void
lock_check(void)
{
    uint32_t i;
    uint32_t spin;
    uint64_t expect = (uint64_t)HAMMER_ROUNDS * cpu_count();

    cnprintf("locks: %s\n", cpu_count() > 1 ?
      "two cores through one region" :
      "one core, which section 13.3 gives no lock to need");

    md_cpu_fence();
    hammer_go = 1;

    for (i = 0; i < HAMMER_ROUNDS; i++) {
        hammer_once();
    }

    if (cpu_count() > 1) {
        for (spin = 0; spin < 100000000u && hammer_done == 0; spin++) {
            md_cpu_relax();
        }
        if (hammer_done == 0) {
            panic("lock check: the second core never finished");
        }
        md_cpu_fence();
    }

    if (hammer.hm_count != expect) {
        panic("lock check: %lu rounds of %lu arrived",
          (unsigned long)hammer.hm_count, (unsigned long)expect);
    }
    if (hammer.hm_shadow != hammer.hm_count * 3) {
        panic("lock check: the count and its shadow disagree");
    }

    cnprintf("    %lu rounds, ", (unsigned long)hammer.hm_count);
    for (i = 0; i < cpu_count(); i++) {
        cnprintf("%lu on core %u%s", (unsigned long)hammer.hm_by[i],
          (unsigned int)i, i + 1 < cpu_count() ? " and " : "");

        /*
         * Both of them, and not one of them twice. A second core that
         * contributed nothing would leave every check above passing,
         * which is the shape of failure this milestone keeps finding.
         */
        if (hammer.hm_by[i] == 0) {
            panic("lock check: core %u went through none of it",
              (unsigned int)i);
        }
    }
    cnprintf("\n");
}
