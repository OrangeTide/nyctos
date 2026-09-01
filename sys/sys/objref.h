/*
 * SPDX-License-Identifier: 0BSD OR Apache-2.0
 *
 * PUBLIC DOMAIN (CC0-1.0)
 */

/*
 * Exclusion for the reference counts that decide when an object is
 * freed.
 *
 * `p_refs`, `t_refs` and `cn_refs` were plain increments. M8 listed
 * them as unsafe the moment there were two cores, M9 listed them again,
 * M10 listed them a third time, and each milestone wrote them down
 * rather than fixing them. The hazard is not a lost count: it is that a
 * count reaching zero is the signal to free, so two cores decrementing
 * together either free the same object twice or neither of them frees
 * it, and M7's whole lesson was that a freed object turns every pointer
 * to it into a way of naming its replacement.
 *
 * An atomic counter is not enough on its own. Every one of the three
 * decides to free by reading the count *and* other state: whether the
 * process has exited and how many threads it has left, whether a thread
 * is reapable and is not the one running. The decision has to be
 * indivisible, not just the arithmetic.
 *
 * So the count and the decision are made under this, and the freeing is
 * done after it is released. That is not tidiness. Reaping a process
 * closes every handle it held, which releases references to other
 * objects, which comes back here: holding this across a reap would be a
 * core taking a lock it already holds, which the lock refuses rather
 * than deadlocking silently.
 *
 * Releasing early is safe because exactly one caller can see the
 * decision become true. Nothing can take a new reference to an object
 * whose count has reached zero, because taking one means holding a
 * handle and there are none left.
 */

#ifndef _SYS_OBJREF_H_
#define _SYS_OBJREF_H_

#include <sys/intr.h>

intr_state_t objref_enter(void);
void objref_leave(intr_state_t saved);

#endif /* _SYS_OBJREF_H_ */
