/*
 * SPDX-License-Identifier: 0BSD OR Apache-2.0
 * Copyright (C) 2026 Jon Mayo <jon@rm-f.net>
 */

/*
 * Interrupts, and the critical sections they force.
 *
 * Section 13.2: an interrupt service routine advances a timeline and
 * returns, and that is the whole of it. It runs no driver logic, takes no
 * lock a thread can hold, and allocates nothing. A driver waits on a
 * timeline whether it is waiting for a device, a timer or a transfer.
 *
 * Section 13.3 makes Tier 0 disable interrupts inside the kernel rather
 * than lock, which is what buys a kernel with no internal locking at all.
 * That is what md_intr_disable is for: the run queues and the timeline
 * waiter lists are touched by both threads and interrupt handlers, and
 * nothing else keeps them apart.
 */

#ifndef _SYS_INTR_H_
#define _SYS_INTR_H_

#include <sys/config.h>
#include <sys/op.h>

struct timeline;

typedef unsigned long intr_state_t;

/*
 * Disable interrupts and return what the state was, so that a nested
 * region restores to disabled rather than to enabled. Every critical
 * section in the kernel is a pair of these.
 */
intr_state_t md_intr_disable(void);
void md_intr_restore(intr_state_t saved);
void md_intr_enable(void);

/* Set up the interrupt controller. Interrupts stay masked until enabled. */
void md_intr_init(void);

/* Wait for an interrupt. Used only by the idle path. */
void md_idle_wait(void);

/* Unmask one interrupt at the controller. */
void md_irq_unmask(unsigned int irq);

/*
 * Acknowledge whatever is pending and return its number, or IRQ_NONE.
 * Called by the trap path before irq_dispatch and paired with md_irq_eoi.
 */
#define IRQ_NONE  ((unsigned int)~0u)

unsigned int md_irq_claim(void);
void md_irq_eoi(unsigned int irq);

/*
 * The timer, which is the only device this stage drives. Section 14 has a
 * one-shot programmed to the earliest pending event and no tick; that
 * needs a clock and a deadline, which is stage 7. This is the crude form:
 * fire once, this many nanoseconds from now.
 */
void md_timer_init(void);
void md_timer_arm(uint64_t nanoseconds);
unsigned int md_timer_irq(void);

/*
 * Bind an interrupt to a timeline. Section 13.2 has this take a handle
 * carrying SYS_RIGHT_SIGNAL; it is kernel-internal here because there is
 * no opcode for it, which is a gap in the design rather than a decision.
 */
sys_err_t irq_attach(unsigned int irq, struct timeline *tl);

/* Called from the trap path with interrupts already disabled by hardware. */
void irq_dispatch(void);

void irq_init(void);

#endif /* _SYS_INTR_H_ */
