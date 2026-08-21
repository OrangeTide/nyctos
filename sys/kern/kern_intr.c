/*
 * SPDX-License-Identifier: 0BSD OR Apache-2.0
 * Copyright (C) 2026 Jon Mayo <jon@rm-f.net>
 */

/*
 * The interrupt path: claim, silence, advance a timeline, acknowledge,
 * return.
 *
 * There is deliberately almost nothing here. Section 13.2 keeps driver
 * logic out of the handler entirely, so what an interrupt does is make a
 * counter go up, and the thread waiting on that counter does the work.
 *
 * Silencing is the part section 13.2 does not describe and every device
 * that is not a timer needs. The section requires the handler to have
 * finished with the device before it advances anything, because advancing
 * can switch to the waiter from inside the handler, and a device still
 * asserting is re-entered the moment that thread enables interrupts. A
 * timer is finished with by reprogramming it, which is kernel code. A UART
 * is finished with by reading the bytes out of it, which is driver logic
 * and belongs in a process. What the kernel can do for a device it does
 * not understand is mask the source, and let the driver unmask it when it
 * has drained the device.
 */

#include <sys/intr.h>
#include <sys/sched.h>
#include <sys/systm.h>
#include <sys/timeline.h>

#include <stdbool.h>
#include <stdint.h>

/*
 * One of these per interrupt, allocated statically and for ever. It is the
 * object a SYS_OBJ_IRQ handle names, so it must outlive any handle to it,
 * which under static pools it trivially does.
 */
struct irq {
    struct timeline *ib_timeline;
    void           (*ib_handler)(void);
    uint64_t         ib_count;
    unsigned int     ib_number;
    bool             ib_masked;
};

static struct irq irq_table[SYS_MAX_IRQS];

void
irq_init(void)
{
    unsigned int i;

    for (i = 0; i < SYS_MAX_IRQS; i++) {
        irq_table[i].ib_timeline = NULL;
        irq_table[i].ib_handler = NULL;
        irq_table[i].ib_count = 0;
        irq_table[i].ib_number = i;
        irq_table[i].ib_masked = false;
    }
}

struct irq *
irq_object(unsigned int irq)
{
    if (irq >= SYS_MAX_IRQS) {
        return NULL;
    }

    return &irq_table[irq];
}

sys_err_t
irq_attach(unsigned int irq, struct timeline *tl)
{
    intr_state_t s;

    if (irq >= SYS_MAX_IRQS || tl == NULL) {
        return SYS_ERR_INVALID_ARG;
    }

    s = md_intr_disable();
    if (irq_table[irq].ib_timeline != NULL || irq_table[irq].ib_handler != NULL) {
        md_intr_restore(s);
        return SYS_ERR_INVALID_ARG;
    }
    irq_table[irq].ib_timeline = tl;
    md_intr_restore(s);

    md_irq_unmask(irq);

    return SYS_OK;
}

sys_err_t
irq_attach_handler(unsigned int irq, void (*fn)(void))
{
    intr_state_t s;

    if (irq >= SYS_MAX_IRQS || fn == NULL) {
        return SYS_ERR_INVALID_ARG;
    }

    s = md_intr_disable();
    if (irq_table[irq].ib_timeline != NULL || irq_table[irq].ib_handler != NULL) {
        md_intr_restore(s);
        return SYS_ERR_INVALID_ARG;
    }
    irq_table[irq].ib_handler = fn;
    md_intr_restore(s);

    md_irq_unmask(irq);

    return SYS_OK;
}

/*
 * The driver has drained its device and wants the source live again.
 *
 * Unmasking one that is not masked is refused rather than ignored,
 * because the only way to reach that state is a driver acknowledging an
 * interrupt it was not given, and a driver that does so has lost track of
 * how many it has seen.
 */
sys_err_t
irq_unmask(struct irq *ir)
{
    intr_state_t s;

    if (ir == NULL) {
        return SYS_ERR_INVALID_ARG;
    }
    if (ir->ib_timeline == NULL) {
        return SYS_ERR_INVALID_ARG;
    }

    s = md_intr_disable();
    if (!ir->ib_masked) {
        md_intr_restore(s);
        return SYS_ERR_INVALID_ARG;
    }
    ir->ib_masked = false;
    md_intr_restore(s);

    md_irq_unmask(ir->ib_number);

    return SYS_OK;
}

/*
 * The whole of an interrupt.
 *
 * The count is what the timeline is advanced to, so the value a waiter
 * asks for is the number of interrupts it wants to have seen. That makes a
 * waiter that arrives late safe in the way section 9.1 describes: the
 * counter has already passed the value and the wait returns at once,
 * rather than the interrupt having been missed.
 */
void
irq_dispatch(void)
{
    struct irq *b;
    unsigned int irq;

    irq = md_irq_claim();
    if (irq == IRQ_NONE) {
        return;
    }

    /*
     * The device is finished with before anything else happens, because
     * advancing a timeline can wake a higher-priority thread and switch to
     * it from right here. Leaving a still-asserting device behind means
     * that thread re-enters this handler the moment it enables interrupts,
     * and the nested trap overwrites the return state of the one already
     * in progress.
     */
    md_irq_eoi(irq);

    if (irq >= SYS_MAX_IRQS) {
        return;
    }

    b = &irq_table[irq];
    if (b->ib_handler != NULL) {
        b->ib_handler();
    } else if (b->ib_timeline != NULL) {
        /*
         * Masked before the advance and not after it. The advance can
         * switch away from here, and a source left live is taken again on
         * the way out of that switch.
         */
        b->ib_masked = true;
        md_irq_mask(irq);

        b->ib_count++;
        (void)timeline_signal(b->ib_timeline, b->ib_count);
    }
}
