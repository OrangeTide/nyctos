/*
 * SPDX-License-Identifier: 0BSD OR Apache-2.0
 * Copyright (C) 2026 Jon Mayo <jon@rm-f.net>
 */

/*
 * The core-local interruptor, as QEMU's virt board presents it.
 *
 * The kernel runs in machine mode, so the machine timer is reached
 * directly and there is no external controller in the way. That makes the
 * timer the easiest first device here for the same reason the private
 * peripheral timer is on the ARM side.
 */

#include <sys/intr.h>
#include <sys/systm.h>

#include <stdint.h>

#define CLINT_MTIMECMP  0x02004000UL    /* hart 0 */
#define CLINT_MTIME     0x0200bff8UL

/* QEMU's virt board runs the machine timer at 10 MHz. */
#define TIMER_HZ        10000000ULL

#define MSTATUS_MIE     (1UL << 3)
#define MIE_MTIE        (1UL << 7)

/*
 * The machine timer has no interrupt number in the sense a controller
 * would give it, so it gets one of its own. Nothing outside this file
 * depends on the value.
 */
#define TIMER_IRQ       7

static volatile uint64_t *const mtime = (volatile uint64_t *)CLINT_MTIME;
static volatile uint64_t *const mtimecmp = (volatile uint64_t *)CLINT_MTIMECMP;

/* Set by the trap path before irq_dispatch; see md_irq_claim. */
unsigned long md_pending_irq;

intr_state_t
md_intr_disable(void)
{
    intr_state_t s;

    __asm__ volatile ("csrrc %0, mstatus, %1"
      : "=r" (s) : "r" (MSTATUS_MIE) : "memory");

    return s & MSTATUS_MIE;
}

void
md_intr_restore(intr_state_t saved)
{
    if (saved != 0) {
        __asm__ volatile ("csrs mstatus, %0" :: "r" (MSTATUS_MIE) : "memory");
    }
}

void
md_intr_enable(void)
{
    __asm__ volatile ("csrs mstatus, %0" :: "r" (MSTATUS_MIE) : "memory");
}

void
md_intr_init(void)
{
    /* Nothing to program: the timer is reached directly from machine mode. */
}

void
md_irq_unmask(unsigned int irq)
{
    if (irq == TIMER_IRQ) {
        __asm__ volatile ("csrs mie, %0" :: "r" (MIE_MTIE) : "memory");
    }
}

unsigned int
md_irq_claim(void)
{
    unsigned long irq = md_pending_irq;

    md_pending_irq = IRQ_NONE;

    return (unsigned int)irq;
}

/*
 * The machine timer is acknowledged by moving its compare value, which
 * md_timer_arm does. Leaving it unmoved would re-enter the handler for
 * ever, so a timer that is not re-armed is disabled instead.
 */
void
md_irq_eoi(unsigned int irq)
{
    if (irq == TIMER_IRQ) {
        *mtimecmp = ~(uint64_t)0;
    }
}

void
md_timer_init(void)
{
    *mtimecmp = ~(uint64_t)0;
}

unsigned int
md_timer_irq(void)
{
    return TIMER_IRQ;
}

void
md_timer_arm(uint64_t nanoseconds)
{
    uint64_t ticks = (TIMER_HZ * nanoseconds) / 1000000000ULL;

    if (ticks == 0) {
        ticks = 1;
    }

    *mtimecmp = *mtime + ticks;
}

void
md_idle_wait(void)
{
    __asm__ volatile ("wfi");
}
