/* thread_md.c : AArch64 initial stack frame for a new thread.
 *
 * SPDX-License-Identifier: 0BSD OR Apache-2.0
 * Copyright (C) 2026 Jon Mayo <jon@rm-f.net>
 *
 * md_thread_init builds a 96-byte frame at the top of a new thread's
 * kernel stack. The frame holds the entry point in x19, its argument in
 * x20, and md_thread_start in x30, the address md_switch returns to. The
 * first switch into the thread therefore begins at md_thread_start.
 *
 */

#include <sys/systm.h>
#include <sys/thread.h>

#include <stdint.h>

#define FRAME_WORDS     12      /* 96 bytes, and 16-byte aligned */
#define SLOT_X19         0      /* entry point, read by md_thread_start */
#define SLOT_X20         1      /* entry argument */
#define SLOT_X30        11      /* where md_switch's ret goes */

void md_thread_start(void);

void
md_thread_init(struct thread *t, void (*entry)(void *), void *arg)
{
    uint64_t *frame;

    frame = (uint64_t *)(t->t_stack + SYS_KSTACK_SIZE) - FRAME_WORDS;
    memset(frame, 0, FRAME_WORDS * sizeof(frame[0]));

    frame[SLOT_X19] = (uint64_t)(uintptr_t)entry;
    frame[SLOT_X20] = (uint64_t)(uintptr_t)arg;
    frame[SLOT_X30] = (uint64_t)(uintptr_t)md_thread_start;

    t->t_sp = frame;
}
