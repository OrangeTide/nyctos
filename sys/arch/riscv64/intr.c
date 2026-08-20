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

#include <stdbool.h>
#include <stdint.h>

#define CLINT_MTIMECMP  0x02004000UL    /* hart 0 */
#define CLINT_MTIME     0x0200bff8UL

/*
 * The platform-level interrupt controller, which is where every device
 * that is not the timer arrives. Context 0 is hart 0 in machine mode,
 * which is the only context this kernel has.
 */
#define PLIC_BASE       0x0c000000UL
#define PLIC_PRIORITY   0x000000        /* one word per source */
#define PLIC_ENABLE     0x002000        /* one bit per source, per context */
#define PLIC_THRESHOLD  0x200000        /* per context */
#define PLIC_CLAIM      0x200004        /* per context */

/* The 16550 on the virt board. */
#define UART_SOURCE     10

/* QEMU's virt board runs the machine timer at 10 MHz. */
#define TIMER_HZ        10000000ULL

#define MSTATUS_MIE     (1UL << 3)
#define MIE_MTIE        (1UL << 7)
#define MIE_MEIE        (1UL << 11)

/*
 * The machine timer has no interrupt number in the sense a controller
 * would give it, so it gets one of its own. Nothing outside this file
 * depends on the value.
 */
#define TIMER_IRQ       7

/*
 * Interrupt numbers above here are PLIC sources, and below here they
 * are mcause codes. The two spaces are unrelated and both start at
 * small numbers, so a source is offset rather than left to collide
 * with the timer. Nothing outside this file knows the arithmetic.
 */
#define PLIC_IRQ_BASE   32

/* mcause code for a machine external interrupt, which is what the PLIC is. */
#define CAUSE_EXTERNAL  11

static volatile uint64_t *const mtime = (volatile uint64_t *)CLINT_MTIME;
static volatile uint64_t *const mtimecmp = (volatile uint64_t *)CLINT_MTIMECMP;

/* Set by the trap path before irq_dispatch; see md_irq_claim. */
unsigned long md_pending_irq;

/* PLIC registers. Context 0 throughout, which is hart 0 in machine mode. */
static volatile uint32_t *
plic_reg(unsigned long off)
{
    return (volatile uint32_t *)(PLIC_BASE + off);
}

static void
plic_enable(unsigned int source, bool on)
{
    volatile uint32_t *reg = plic_reg(PLIC_ENABLE + (source / 32) * 4);
    uint32_t bit = (uint32_t)1 << (source % 32);

    if (on) {
        *reg |= bit;
    } else {
        *reg &= ~bit;
    }
}

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
    /*
     * The timer needs nothing: it is reached from machine mode
     * directly. The PLIC needs its source given a priority above the
     * context threshold, or it delivers nothing and says nothing.
     */
    *plic_reg(PLIC_THRESHOLD) = 0;
    *plic_reg(PLIC_PRIORITY + UART_SOURCE * 4) = 1;
}
void
md_irq_unmask(unsigned int irq)
{
    if (irq >= PLIC_IRQ_BASE) {
        plic_enable(irq - PLIC_IRQ_BASE, true);
        __asm__ volatile ("csrs mie, %0" :: "r" (MIE_MEIE) : "memory");
    } else if (irq == TIMER_IRQ) {
        __asm__ volatile ("csrs mie, %0" :: "r" (MIE_MTIE) : "memory");
    }
}

void
md_irq_mask(unsigned int irq)
{
    if (irq >= PLIC_IRQ_BASE) {
        plic_enable(irq - PLIC_IRQ_BASE, false);
    }
}

unsigned int
md_uart_irq(void)
{
    return PLIC_IRQ_BASE + UART_SOURCE;
}

unsigned int
md_irq_claim(void)
{
    unsigned long cause = md_pending_irq;
    uint32_t source;

    md_pending_irq = IRQ_NONE;

    /*
     * A machine external interrupt says only that the controller has
     * something. Which source it is comes from the claim register, and
     * reading it is what stops the PLIC re-raising while it is served.
     */
    if (cause == CAUSE_EXTERNAL) {
        source = *plic_reg(PLIC_CLAIM);
        if (source == 0) {
            return IRQ_NONE;
        }
        return PLIC_IRQ_BASE + source;
    }

    return (unsigned int)cause;
}

/*
 * Finished with. A PLIC source is completed by writing its number back
 * to the claim register, and the machine timer is acknowledged by
 * moving its compare value, which md_timer_arm does. Leaving it
 * unmoved would re-enter the handler for ever, so a timer that is not
 * re-armed is disabled instead.
 */
void
md_irq_eoi(unsigned int irq)
{
    if (irq >= PLIC_IRQ_BASE) {
        *plic_reg(PLIC_CLAIM) = irq - PLIC_IRQ_BASE;
    } else if (irq == TIMER_IRQ) {
        md_timer_disarm();
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
md_idle_wait(void)
{
    __asm__ volatile ("wfi");
}

uint64_t
md_time_now(void)
{
    return *mtime;
}

uint64_t
md_time_hz(void)
{
    return TIMER_HZ;
}

void
md_timer_at(uint64_t when)
{
    *mtimecmp = when;
}

void
md_timer_disarm(void)
{
    *mtimecmp = ~(uint64_t)0;
}
