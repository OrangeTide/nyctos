/* intr.c : RISC-V PLIC, Sstc timer, and inter-hart interrupts.
 *
 * SPDX-License-Identifier: 0BSD OR Apache-2.0
 * Copyright (C) 2026 Jon Mayo <jon@rm-f.net>
 *
 * The PLIC routes external interrupts, and each hart claims and completes
 * them from its own supervisor context. The timer is the supervisor
 * compare register from the Sstc extension, so no machine-mode handler is
 * involved. An inter-processor interrupt is a write to the CLINT MSIP
 * word of the target hart, which boot.S turns into SSIP.
 *
 */

#include <sys/cpu.h>
#include <sys/measure.h>
#include <sys/intr.h>
#include <sys/systm.h>

#include <stdbool.h>
#include <stdint.h>


/*
 * The platform-level interrupt controller, which is where every device
 * that is not the timer arrives.
 * A context is a hart and a privilege level together: hart H has
 * context 2H for its machine mode and 2H+1 for its supervisor mode.
 * Everything per-context here is therefore addressed from the core
 * asking, and was addressed as hart 0 until M10. Nothing had gone
 * wrong, because only the first core ever enables an external source
 * and so only it ever claims one; a second core that took an external
 * interrupt would have claimed from the first core's context and taken
 * an interrupt that was not delivered to it.
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

#define SSTATUS_SIE     (1UL << 1)
#define SIE_SSIE        (1UL << 1)
#define SIP_SSIP        (1UL << 1)
#define SIE_STIE        (1UL << 5)
#define SIE_SEIE        (1UL << 9)

/*
 * The supervisor timer has no interrupt number in the sense a
 * controller would give it, so it gets one of its own, which is its
 * cause code. Nothing outside this file depends on the value.
 */
#define TIMER_IRQ       5               /* supervisor timer */

/*
 * The supervisor software interrupt, which is what a poke from another
 * hart turns into once machine mode has handed it over. See boot.S.
 */
#define IPI_IRQ         1
#define CLINT_MSIP      0x02000000UL

/*
 * Interrupt numbers above here are PLIC sources, and below here they
 * are mcause codes. The two spaces are unrelated and both start at
 * small numbers, so a source is offset rather than left to collide
 * with the timer. Nothing outside this file knows the arithmetic.
 */
#define PLIC_IRQ_BASE   32

/* scause code for a supervisor external interrupt, which is the PLIC. */
#define CAUSE_EXTERNAL  9               /* supervisor external */

/* Set by the trap path before irq_dispatch; see md_irq_claim. */
unsigned long md_pending_irq;

/* PLIC registers. Context 0 throughout, which is hart 0 in machine mode. */
static unsigned int
plic_context(void)
{
    /*
     * This core's supervisor context. The core index is the hart
     * number on this port: a hart carries its own number down from
     * machine mode and claims the entry of that index.
     */
    return 2u * cpu_self()->cp_index + 1u;
}

static unsigned long
plic_enable_ctx(void)
{
    return PLIC_ENABLE + (unsigned long)plic_context() * 0x80UL;
}

static unsigned long
plic_hart_ctx(void)
{
    return (unsigned long)plic_context() * 0x1000UL;
}

static volatile uint32_t *
plic_reg(unsigned long off)
{
    return (volatile uint32_t *)(PLIC_BASE + off);
}

static void
plic_enable(unsigned int source, bool on)
{
    volatile uint32_t *reg = plic_reg(plic_enable_ctx() + (source / 32) * 4);
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

    __asm__ volatile ("csrrc %0, sstatus, %1"
      : "=r" (s) : "r" (SSTATUS_SIE) : "memory");

    s &= SSTATUS_SIE;

    /*
     * Only the call that actually turns them off begins a region.
     * One that finds them already off is inside a region somebody
     * else began, and section 13.3 asks about the contiguous one.
     */
    if (s != 0) {
        intr_region_begin((uintptr_t)__builtin_return_address(0));
    }

    return s;
}

void
md_intr_restore(intr_state_t saved)
{
    if (saved != 0) {
        intr_region_end((uintptr_t)__builtin_return_address(0));
        __asm__ volatile ("csrs sstatus, %0" :: "r" (SSTATUS_SIE) : "memory");
    }
}

void
md_intr_enable(void)
{
    /*
     * An explicit enable ends the region as surely as a restore does.
     * Without this the span runs on until some later restore closes
     * it, and the figure includes time when interrupts were on.
     */
    intr_region_end((uintptr_t)__builtin_return_address(0));

    __asm__ volatile ("csrs sstatus, %0" :: "r" (SSTATUS_SIE) : "memory");
}

void
md_intr_init(void)
{
    /*
     * The timer needs nothing: supervisor mode programs its own. The
     * PLIC needs its source given a priority above the threshold of
     * the context it is delivering to, or it delivers nothing and says
     * nothing.
     *
     * A source priority is one word for the machine. The threshold is
     * per context and has moved to the per-core half below, where a
     * core that is not the first one also gets one.
     */
    *plic_reg(PLIC_PRIORITY + UART_SOURCE * 4) = 1;
}
void
md_irq_unmask(unsigned int irq)
{
    if (irq >= PLIC_IRQ_BASE) {
        plic_enable(irq - PLIC_IRQ_BASE, true);
        __asm__ volatile ("csrs sie, %0" :: "r" (SIE_SEIE) : "memory");
    } else if (irq == TIMER_IRQ) {
        __asm__ volatile ("csrs sie, %0" :: "r" (SIE_STIE) : "memory");
    } else if (irq == IPI_IRQ) {
        __asm__ volatile ("csrs sie, %0" :: "r" (SIE_SSIE) : "memory");
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
    /*
     * Taken down here, because the bit machine mode raised stays raised
     * until supervisor mode clears it and a handler that returns
     * without doing so is entered again at once.
     */
    if (cause == IPI_IRQ) {
        __asm__ volatile ("csrc sip, %0" :: "r" (SIP_SSIP) : "memory");
        return IPI_IRQ;
    }

    if (cause == CAUSE_EXTERNAL) {
        source = *plic_reg(PLIC_CLAIM + plic_hart_ctx());
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
        *plic_reg(PLIC_CLAIM + plic_hart_ctx()) = irq - PLIC_IRQ_BASE;
    } else if (irq == TIMER_IRQ) {
        md_timer_disarm();
    }
}

void
md_timer_init(void)
{
    md_timer_disarm();
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
    uint64_t v;

    /*
     * The time counter, read as a counter rather than as a device.
     * Machine mode permits it through mcounteren; without that this
     * instruction is an illegal one.
     */
    __asm__ volatile ("rdtime %0" : "=r" (v));

    return v;
}
uint64_t
md_time_hz(void)
{
    return TIMER_HZ;
}

void
md_timer_at(uint64_t when)
{
    /*
     * stimecmp, which supervisor mode owns because machine mode set
     * menvcfg.STCE at boot. The number is used rather than the name so
     * that an assembler which has not heard of Sstc still assembles
     * it, which is a real risk for an extension this recent.
     */
    __asm__ volatile ("csrw 0x14d, %0" :: "r" (when));
}
void
md_timer_disarm(void)
{
    __asm__ volatile ("csrw 0x14d, %0" :: "r" (~(uint64_t)0));
}

unsigned int
md_cpu_ipi_irq(void)
{
    return IPI_IRQ;
}

void
md_cpu_ipi(uint32_t index)
{
    volatile uint32_t *msip = (volatile uint32_t *)(uintptr_t)CLINT_MSIP;

    /*
     * A machine software interrupt on that hart, which its machine mode
     * hands to its supervisor mode. There is no firmware here to ask,
     * so boot.S does what firmware would.
     */
    msip[index] = 1;
}

void
md_intr_init_core(void)
{
    /*
     * This core's threshold, so that a source enabled in this core's
     * context can reach it. Every context starts with a threshold that
     * refuses everything, and a core whose threshold nobody lowered
     * would be enabled for a source and never delivered one.
     */
    *plic_reg(PLIC_THRESHOLD + plic_hart_ctx()) = 0;

    /*
     * And that it is this core's and not the first core's. Every
     * per-context register here was addressed as hart zero until M10,
     * so a second core setting its threshold was setting the first
     * core's, and a second core claiming an interrupt would have taken
     * one delivered to the first.
     *
     * Written and read back rather than reasoned about, because the
     * arithmetic that was wrong is the arithmetic that would be
     * checked. A value nothing else uses, into this core's threshold,
     * and then the first core's is read: if they are the same register
     * it comes back.
     */
    if (cpu_self()->cp_index != 0) {
        *plic_reg(PLIC_THRESHOLD + plic_hart_ctx()) = 7;

        if (*plic_reg(PLIC_THRESHOLD + 0x1000UL) == 7) {
            panic("plic: core %u shares the first core's context",
              (unsigned int)cpu_self()->cp_index);
        }

        *plic_reg(PLIC_THRESHOLD + plic_hart_ctx()) = 0;
    }

    /*
     * And the poke. The timer and the external controller are unmasked
     * by whoever attaches to them, and a core that runs nothing
     * attaches to neither; being reachable by the other core is what
     * every core needs from the start.
     */
    md_irq_unmask(IPI_IRQ);
}
