/*
 * SPDX-License-Identifier: 0BSD OR Apache-2.0
 * Copyright (C) 2026 Jon Mayo <jon@rm-f.net>
 */

/*
 * GICv2 and the ARM generic timer, as QEMU's virt board presents them.
 *
 * The timer is a private peripheral interrupt, so it needs no routing and
 * no distributor target setting: it is per-core by construction, which is
 * why it is the easiest first device.
 */

#include <sys/intr.h>
#include <sys/systm.h>

#include <stdint.h>

#define GICD_BASE       0x08000000UL
#define GICC_BASE       0x08010000UL

#define GICD_CTLR       0x000
#define GICD_ISENABLER  0x100

#define GICC_CTLR       0x000
#define GICC_PMR        0x004
#define GICC_IAR        0x00c
#define GICC_EOIR       0x010

/* The EL1 physical timer, INTID 30. */
#define TIMER_PPI       30

/* GICv2 returns this from IAR when there is nothing pending. */
#define GIC_SPURIOUS    1023

static uint64_t timer_hz;

static void
mmio_write(unsigned long base, unsigned int off, uint32_t v)
{
    *(volatile uint32_t *)(base + off) = v;
}

static uint32_t
mmio_read(unsigned long base, unsigned int off)
{
    return *(volatile uint32_t *)(base + off);
}

intr_state_t
md_intr_disable(void)
{
    intr_state_t daif;

    __asm__ volatile ("mrs %0, daif" : "=r" (daif));
    __asm__ volatile ("msr daifset, #2" ::: "memory");

    return daif;
}

void
md_intr_restore(intr_state_t saved)
{
    __asm__ volatile ("msr daif, %0" :: "r" (saved) : "memory");
}

void
md_intr_enable(void)
{
    __asm__ volatile ("msr daifclr, #2" ::: "memory");
}

void
md_intr_init(void)
{
    mmio_write(GICD_BASE, GICD_CTLR, 1);

    /* Accept every priority, then let the CPU interface deliver. */
    mmio_write(GICC_BASE, GICC_PMR, 0xff);
    mmio_write(GICC_BASE, GICC_CTLR, 1);
}

void
md_irq_unmask(unsigned int irq)
{
    unsigned int reg = (irq / 32) * 4;

    mmio_write(GICD_BASE, GICD_ISENABLER + reg, (uint32_t)1 << (irq % 32));
}

unsigned int
md_irq_claim(void)
{
    uint32_t iar = mmio_read(GICC_BASE, GICC_IAR);
    unsigned int irq = iar & 0x3ff;

    if (irq == GIC_SPURIOUS) {
        return IRQ_NONE;
    }

    return irq;
}

/*
 * The generic timer asserts for as long as its compare value is in the
 * past, so acknowledging at the controller is not enough: without
 * disarming it here the same interrupt is taken again the instant the
 * handler returns, for ever. A timer that is not re-armed is disabled,
 * which is the same rule the RISC-V port follows by moving its compare
 * value out of reach.
 */
void
md_irq_eoi(unsigned int irq)
{
    if (irq == TIMER_PPI) {
        __asm__ volatile ("msr cntp_ctl_el0, %0" :: "r" (0UL));
    }

    mmio_write(GICC_BASE, GICC_EOIR, (uint32_t)irq);
}

void
md_timer_init(void)
{
    __asm__ volatile ("mrs %0, cntfrq_el0" : "=r" (timer_hz));
    if (timer_hz == 0) {
        panic("timer: the counter reports no frequency");
    }
}

unsigned int
md_timer_irq(void)
{
    return TIMER_PPI;
}

/*
 * Arm once. Writing the down-counter rather than a compare value is the
 * simpler of the two interfaces and is all a fire-once needs; section 14
 * wants an absolute deadline, and that arrives with a clock to state one
 * against.
 */
void
md_timer_arm(uint64_t nanoseconds)
{
    uint64_t ticks = (timer_hz * nanoseconds) / 1000000000ULL;

    if (ticks == 0) {
        ticks = 1;
    }

    __asm__ volatile ("msr cntp_tval_el0, %0" :: "r" (ticks));
    __asm__ volatile ("msr cntp_ctl_el0, %0" :: "r" (1UL));
}

void
md_idle_wait(void)
{
    __asm__ volatile ("wfi");
}
