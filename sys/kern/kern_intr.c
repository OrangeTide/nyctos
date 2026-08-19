/*
 * SPDX-License-Identifier: 0BSD OR Apache-2.0
 * Copyright (C) 2026 Jon Mayo <jon@rm-f.net>
 */

/*
 * The interrupt path: claim, advance a timeline, acknowledge, return.
 *
 * There is deliberately almost nothing here. Section 13.2 keeps driver
 * logic out of the handler entirely, so what an interrupt does is make a
 * counter go up, and the thread waiting on that counter does the work.
 */

#include <sys/intr.h>
#include <sys/sched.h>
#include <sys/systm.h>
#include <sys/timeline.h>

struct irq_binding {
    struct timeline *ib_timeline;
    uint64_t         ib_count;
};

static struct irq_binding irq_table[SYS_MAX_IRQS];

void
irq_init(void)
{
    unsigned int i;

    for (i = 0; i < SYS_MAX_IRQS; i++) {
        irq_table[i].ib_timeline = NULL;
        irq_table[i].ib_count = 0;
    }
}

sys_err_t
irq_attach(unsigned int irq, struct timeline *tl)
{
    intr_state_t s;

    if (irq >= SYS_MAX_IRQS || tl == NULL) {
        return SYS_ERR_INVALID_ARG;
    }

    s = md_intr_disable();
    if (irq_table[irq].ib_timeline != NULL) {
        md_intr_restore(s);
        return SYS_ERR_INVALID_ARG;
    }
    irq_table[irq].ib_timeline = tl;
    md_intr_restore(s);

    md_irq_unmask(irq);

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
    struct irq_binding *b;
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

    if (irq < SYS_MAX_IRQS && irq_table[irq].ib_timeline != NULL) {
        b = &irq_table[irq];
        b->ib_count++;
        (void)timeline_signal(b->ib_timeline, b->ib_count);
    }
}
