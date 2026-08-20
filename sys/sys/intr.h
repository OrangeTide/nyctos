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
 * Mask one interrupt at the controller, so that a source the handler
 * cannot finish with stops asserting. See irq_dispatch for why every
 * interrupt bound to a timeline is masked before it is delivered.
 */
void md_irq_mask(unsigned int irq);

/*
 * The console UART's receive interrupt. The number is the port's business
 * and nothing above here knows what it means; what matters is that there
 * is exactly one of them and that the port can name it.
 */
unsigned int md_uart_irq(void);

/*
 * Acknowledge whatever is pending and return its number, or IRQ_NONE.
 * Called by the trap path before irq_dispatch and paired with md_irq_eoi.
 */
#define IRQ_NONE  ((unsigned int)~0u)

unsigned int md_irq_claim(void);
void md_irq_eoi(unsigned int irq);

/*
 * The timer. Section 14.1 has no tick: the hardware is programmed one-shot
 * to the earliest event the core knows about, and a core with nothing
 * pending programs no timer at all.
 */
void md_timer_init(void);
unsigned int md_timer_irq(void);

/* The monotonic counter everything is defined against. Section 14.5. */
uint64_t md_time_now(void);
uint64_t md_time_hz(void);

/* Arm the one-shot for an absolute instant, or arm nothing at all. */
void md_timer_at(uint64_t when);
void md_timer_disarm(void);

/*
 * Bind an interrupt to a timeline. Section 13.2 has this take a handle
 * carrying SYS_RIGHT_SIGNAL; it is kernel-internal here because there is
 * no opcode for it, which is a gap in the design rather than a decision.
 */
sys_err_t irq_attach(unsigned int irq, struct timeline *tl);

/*
 * Bind an interrupt to a kernel function instead of a timeline. The timer
 * is the kernel scheduling mechanism of section 14.1 rather than a device
 * a driver waits on, so it is the one interrupt that does not land on a
 * timeline of its own.
 */
sys_err_t irq_attach_handler(unsigned int irq, void (*fn)(void));

/*
 * The interrupt as an object a program can hold a handle to, per section
 * 13.2, and the acknowledgement that unmasks it.
 *
 * A handler that advances a timeline has not finished with the device: the
 * bytes are still in the UART and only the driver knows what to do with
 * them. What the handler can do is silence the source, which it does, and
 * what the driver does when it has drained the device is say so. Without
 * that pair a level-triggered source is re-entered before the handler
 * returns and the machine stops responding rather than failing.
 *
 * The timer does not take this path. Its handler is kernel code that
 * reprograms the device, which is what finishing with it means, so it is
 * never masked and there is nothing to acknowledge.
 */
struct irq;

struct irq *irq_object(unsigned int irq);
sys_err_t irq_ack(struct irq *ir);

/* Called from the trap path with interrupts already disabled by hardware. */
void irq_dispatch(void);

void irq_init(void);

/*
 * Interrupts taken while a program was running, as opposed to while a
 * kernel thread was.
 *
 * It is two instructions on a path that is already the slow one, and it is
 * the only way to tell the two apart from above: an interrupt that arrives
 * in user mode and one that arrives in the kernel run the same handler and
 * leave the same trace. Section 22.2 wants a system that can say what it
 * did, and "no program has ever been interrupted" was true for two
 * milestones without anything being able to notice.
 */
extern unsigned long md_user_irqs;

#endif /* _SYS_INTR_H_ */
