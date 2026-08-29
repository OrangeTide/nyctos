/*
 * SPDX-License-Identifier: 0BSD OR Apache-2.0
 *
 * PUBLIC DOMAIN (CC0-1.0)
 */

/*
 * Mutual exclusion, per section 11.4 and as a stated deviation from
 * section 13.3.
 *
 * Until this milestone there was one core, and disabling interrupts was
 * exclusion: nothing else could be running, so a region that could not
 * be interrupted could not be interfered with either. That reasoning is
 * gone. Disabling interrupts on this core says nothing whatever about
 * what the other core is doing, and every critical section in this
 * kernel was written when it did.
 *
 * So there is a lock, and one of them for the whole kernel. Section
 * 11.4 forbids a global scheduler lock and says nothing about the rest,
 * which is what makes this a resting place rather than a shortcut: the
 * next stage takes the scheduler out from under it because that section
 * requires it, and what else deserves taking out is a measurement.
 *
 * The deviation is from section 13.3, which has Tier 2 enabling
 * interrupts inside the kernel and locking finely. This holds the lock
 * with interrupts disabled, which is coarse in both directions at once.
 * It is recorded rather than hidden, and the section stands: what is
 * built here is the resting place the finer thing gets built from.
 *
 * At Tier 0 none of this exists. Section 13.3 buys that tier a kernel
 * with no internal locking at all, and one core is what makes disabling
 * interrupts exclusion again.
 */

#ifndef _SYS_LOCK_H_
#define _SYS_LOCK_H_

#include <sys/config.h>
#include <sys/intr.h>

#include <stdbool.h>
#include <stdint.h>

#if SYS_TIER == 2

struct spinlock {
    /*
     * Free, or held. Written with an atomic exchange and read on the
     * spin, so it is the one word two cores race for.
     */
    volatile uint32_t   sl_locked;

    /*
     * Which core holds it, plus one, so that zero is nobody. Read only
     * by the holder and by a core that has just failed to take it, and
     * written only under the word above.
     */
    volatile uint32_t   sl_owner;

    const char         *sl_name;
};

/*
 * Take it, spinning until it is free. Taking one this core already
 * holds is refused: the alternative is spinning forever on a word only
 * this core can clear, which is a machine that stops with nothing said.
 */
void spin_lock(struct spinlock *l);

/*
 * Release it. Releasing one that is not held, or one another core
 * holds, stops the machine: the second core would be inside a region
 * this one thought it was alone in, and there is no recovering from
 * having already been wrong about that.
 */
void spin_unlock(struct spinlock *l);

/*
 * The kernel lock, held from the moment a core enters the kernel until
 * it leaves. There are four ways in, all of them from the port's
 * exception vectors, and each one is a pair of these.
 *
 * Two cores are kept apart by the lock and one core is kept out of its
 * own way by the mask it holds the lock under. Those were the same job
 * while there was one core, and they are not.
 *
 * The interrupt state is returned and handed back rather than kept
 * anywhere, because these do nest. A thread that blocks reaches the
 * idle path, which puts the lock down and enables interrupts on
 * purpose, and the interrupt that arrives there enters the kernel
 * inside an entry that has not left yet. One slot per core held the
 * first state until the second entry overwrote it, and the only reason
 * nothing broke is that every entry to this kernel is from a vector
 * that already masked interrupts, so every state saved was the same
 * one.
 */
intr_state_t klock_enter(void);
void klock_exit(intr_state_t saved);

/*
 * Put down where this core stops running and picked up where it starts
 * again, at a context switch and around the idle wait.
 *
 * Not held across either, which is the whole shape of this lock. A
 * thread that blocks in the kernel has already put its state in order
 * by the time it blocks, so the region does not need to survive the
 * block, and the alternatives are both worse: carrying the lock across
 * a switch hands it to a thread that never took it, and holding it
 * through an idle wait is a core asleep with the door shut against the
 * interrupt that would wake it.
 *
 * A kernel thread holds nothing, because it never entered: it started
 * inside. So both of these ask first and say what they found, and the
 * caller gives back exactly what it had.
 */
bool klock_drop(void);
void klock_regain(bool had);

#else

#define klock_enter()           ((intr_state_t)0)
#define klock_exit(saved)       ((void)(saved))
#define klock_drop()            (false)
#define klock_regain(had)       ((void)(had))

#endif /* SYS_TIER == 2 */

/*
 * How many times any core has taken a lock another core could hold.
 * Zero across a scheduling decision is section 11.4's requirement, and
 * it is checked rather than asserted in prose.
 */
#if SYS_TIER == 2
uint64_t spin_take_count(void);
#else
#define spin_take_count()       ((uint64_t)0)
#endif

/*
 * Two cores through the same kernel path at once, which is the only way
 * to find out whether the lock above does anything. Nothing is scheduled
 * on the second core until the next stage, so the two cores here are the
 * boot core and the one that is on its way to the idle loop.
 */
void lock_check(void);
void lock_check_secondary(void);

#endif /* _SYS_LOCK_H_ */
