/*
 * SPDX-License-Identifier: 0BSD OR Apache-2.0
 * Copyright (C) 2026 Jon Mayo <jon@rm-f.net>
 */

#include <sys/object.h>
#include <sys/systm.h>
#include <sys/thread.h>

#define ROUNDS  4

/*
 * Two threads that hand control back and forth by naming each other. There
 * is no scheduler to consult and no timer to preempt them, which is the
 * point of stage 2: the only thing under test is whether a context survives
 * being saved and restored.
 *
 * Each thread holds a value in a local across the switch and checks it
 * afterwards. Whether the compiler keeps that value in a callee-saved
 * register or spills it to the stack, it is testing one of the two things
 * md_switch has to get right, and it costs nothing to check both threads.
 */

#define PING_MAGIC  ((unsigned long)0x1111222233334444UL)
#define PONG_MAGIC  ((unsigned long)0x5555666677778888UL)

static struct thread *t_boot;
static struct thread *t_ping;
static struct thread *t_pong;

static void
ping(void *arg)
{
    unsigned long magic = PING_MAGIC;
    int i;

    (void)arg;

    for (i = 0; i < ROUNDS; i++) {
        cnprintf("  ping %d (on %s)\n", i, thread_current()->t_name);
        thread_switch_to(t_pong);
        if (magic != PING_MAGIC) {
            panic("ping: context not preserved across the switch");
        }
    }

    cnputs("  ping: done, handing back to boot\n");
    thread_switch_to(t_boot);
    panic("ping: ran again after handing back");
}

static void
pong(void *arg)
{
    unsigned long magic = PONG_MAGIC;

    (void)arg;

    for (;;) {
        cnprintf("  pong    (on %s)\n", thread_current()->t_name);
        thread_switch_to(t_ping);
        if (magic != PONG_MAGIC) {
            panic("pong: context not preserved across the switch");
        }
    }
}

void
kernel_main(void)
{
    cnputs("Nyctos/" ARCH_NAME " kernel booting\n");

    object_init();
    object_check();
    object_report();

    thread_bootstrap("boot");
    t_boot = thread_current();
    t_ping = thread_create("ping", ping, NULL);
    t_pong = thread_create("pong", pong, NULL);

    cnputs("threads: switching\n");
    thread_switch_to(t_ping);

    /*
     * Reached only when ping hands back, which means a context was saved
     * here, two others ran on their own stacks, and this one was restored.
     */
    cnprintf("threads: back on %s\n", thread_current()->t_name);

    cnputs("stage 2 complete\n");
    machine_halt(0);
}
