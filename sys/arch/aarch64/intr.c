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

#include <sys/measure.h>
#include <sys/cpu.h>
#include <sys/intr.h>
#include <sys/systm.h>

#include <stdint.h>

#define GICD_BASE       0x08000000UL
#define GICC_BASE       0x08010000UL

#define GICD_CTLR       0x000
#define GICD_ISENABLER  0x100
#define GICD_ICENABLER  0x180
#define GICD_IPRIORITYR 0x400
#define GICD_ITARGETSR  0x800
#define GICD_SGIR       0xf00

#define GICC_CTLR       0x000
#define GICC_PMR        0x004
#define GICC_IAR        0x00c
#define GICC_EOIR       0x010

/* The EL1 physical timer, INTID 30. */
#define TIMER_PPI       30

/*
 * The PL011 on the virt board is shared peripheral interrupt 1, and a
 * shared peripheral interrupt is INTID 32 plus its number. Unlike the
 * timer, which is per-core by construction, it has to be routed to a
 * core and given a priority the CPU interface will accept.
 */
/*
 * The software generated interrupt a core is poked with. Section 11.4
 * has donation cross a boundary this way, and on this port the
 * distributor sends one core an interrupt on behalf of another.
 */
#define IPI_SGI         0

#define UART_SPI        1
#define UART_INTID      (32 + UART_SPI)

/* GICv2 returns this from IAR when there is nothing pending. */
#define GIC_SPURIOUS    1023

/* The interrupt mask bit in DAIF. Set means interrupts are off. */
#define DAIF_I          (1UL << 7)

/* What each core last took from the controller. See md_irq_claim. */
static uint32_t claimed[SYS_MAX_CORES];

static uint64_t timer_hz;

static void
mmio_write(unsigned long base, unsigned int off, uint32_t v)
{
    *(volatile uint32_t *)(base + off) = v;
}

static void
mmio_write8(unsigned long base, unsigned int off, uint8_t v)
{
    *(volatile uint8_t *)(base + off) = v;
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

    /*
     * Only the call that actually turns them off begins a region. The
     * I bit clear means they were on, which makes this the one that
     * started the contiguous region section 13.3 asks about.
     */
    if ((daif & DAIF_I) == 0) {
        intr_region_begin((uintptr_t)__builtin_return_address(0));
    }

    return daif;
}

void
md_intr_restore(intr_state_t saved)
{
    if ((saved & DAIF_I) == 0) {
        intr_region_end();
    }

    __asm__ volatile ("msr daif, %0" :: "r" (saved) : "memory");
}

void
md_intr_enable(void)
{
    /*
     * An explicit enable ends the region as surely as a restore does.
     * Without this the span runs on until some later restore closes
     * it, and the figure includes time when interrupts were on.
     */
    intr_region_end();

    __asm__ volatile ("msr daifclr, #2" ::: "memory");
}

void
md_intr_init(void)
{
    mmio_write(GICD_BASE, GICD_CTLR, 1);
    md_intr_init_core();

    /*
     * A shared peripheral interrupt reaches no core until it is told
     * which one, and QEMU leaves the target register clear. It also
     * needs a priority the CPU interface will accept: the mask above
     * takes everything below 0xff, and 0xff itself is never.
     */
    mmio_write8(GICD_BASE, GICD_ITARGETSR + UART_INTID, 0x01);
    mmio_write8(GICD_BASE, GICD_IPRIORITYR + UART_INTID, 0x80);
}

/*
 * What each core has to do for itself. The distributor is one device
 * for the machine and the CPU interface is one per core, so a core
 * that has not done this receives nothing however well the machine is
 * configured.
 */
void
md_intr_init_core(void)
{
    /* Accept every priority, then let the CPU interface deliver. */
    mmio_write(GICC_BASE, GICC_PMR, 0xff);
    mmio_write(GICC_BASE, GICC_CTLR, 1);

    /*
     * The software generated interrupt this core is poked with. Its
     * priority register is banked per core, so this is the core's own
     * even though the address is not.
     */
    mmio_write8(GICD_BASE, GICD_IPRIORITYR + IPI_SGI, 0x80);
    mmio_write(GICD_BASE, GICD_ISENABLER, (uint32_t)1 << IPI_SGI);
}

unsigned int
md_cpu_ipi_irq(void)
{
    return IPI_SGI;
}

void
md_cpu_ipi(uint32_t index)
{
    mmio_write(GICD_BASE, GICD_SGIR,
      ((uint32_t)1 << (16 + index)) | IPI_SGI);
}

void
md_irq_unmask(unsigned int irq)
{
    unsigned int reg = (irq / 32) * 4;

    mmio_write(GICD_BASE, GICD_ISENABLER + reg, (uint32_t)1 << (irq % 32));
}

void
md_irq_mask(unsigned int irq)
{
    unsigned int reg = (irq / 32) * 4;

    mmio_write(GICD_BASE, GICD_ICENABLER + reg, (uint32_t)1 << (irq % 32));
}

unsigned int
md_uart_irq(void)
{
    return UART_INTID;
}

unsigned int
md_irq_claim(void)
{
    uint32_t iar = mmio_read(GICC_BASE, GICC_IAR);
    unsigned int irq = iar & 0x3ff;

    if (irq == GIC_SPURIOUS) {
        return IRQ_NONE;
    }

    /*
     * Kept whole, because finishing with a software generated
     * interrupt means saying which core sent it. The identifier alone
     * does not: the same interrupt number from two cores is two
     * interrupts to this controller, and an acknowledgement naming the
     * wrong one leaves the right one active.
     *
     * Per core, because two cores are in here independently, and one
     * slot for the machine would have one of them acknowledge the
     * other's interrupt.
     */
    claimed[md_cpu_get_self()->cp_index] = iar;

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
    uint32_t iar = claimed[md_cpu_get_self()->cp_index];

    if (irq == TIMER_PPI) {
        md_timer_disarm();
    }

    /*
     * What was claimed, if this is finishing with what was claimed,
     * which carries the sending core for a software interrupt and is
     * the plain number for everything else.
     */
    if ((iar & 0x3ff) != irq) {
        iar = (uint32_t)irq;
    }

    mmio_write(GICC_BASE, GICC_EOIR, iar);
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



void
md_idle_wait(void)
{
    __asm__ volatile ("wfi");
}

/*
 * The monotonic counter the whole system is defined against. Section 14.5
 * makes everything here uptime, never calendar time, which is what makes a
 * clock correction structurally unable to disturb a deadline.
 */
uint64_t
md_time_now(void)
{
    uint64_t v;

    __asm__ volatile ("isb");
    __asm__ volatile ("mrs %0, cntpct_el0" : "=r" (v));

    return v;
}

uint64_t
md_time_hz(void)
{
    return timer_hz;
}

/*
 * Program the one-shot for an absolute instant. Section 14.1 has no tick:
 * the hardware is armed for the earliest thing the core knows about and
 * for nothing else, so an idle core with nothing pending arms nothing.
 */
void
md_timer_at(uint64_t when)
{
    __asm__ volatile ("msr cntp_cval_el0, %0" :: "r" (when));
    __asm__ volatile ("msr cntp_ctl_el0, %0" :: "r" (1UL));
}

void
md_timer_disarm(void)
{
    __asm__ volatile ("msr cntp_ctl_el0, %0" :: "r" (0UL));
}
