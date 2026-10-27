/* thread_md.c : RISC-V initial stack frame for a new thread.
 *
 * SPDX-License-Identifier: 0BSD OR Apache-2.0
 * Copyright (C) 2026 Jon Mayo <jon@rm-f.net>
 *
 * md_thread_init builds a 112-byte frame at the top of a new thread's
 * kernel stack. The frame holds md_thread_start in ra, the entry point in
 * s0, and its argument in s1. The first switch into the thread therefore
 * begins at md_thread_start.
 *
 */

#include <sys/systm.h>
#include <sys/thread.h>

#include <stdint.h>

#define FRAME_WORDS     14      /* 112 bytes, and 16-byte aligned */
#define SLOT_RA          0      /* where md_switch's ret goes */
#define SLOT_S0          1      /* entry point, read by md_thread_start */
#define SLOT_S1          2      /* entry argument */

void md_thread_start(void);

void
md_thread_init(struct thread *t, void (*entry)(void *), void *arg)
{
    uint64_t *frame;

    frame = (uint64_t *)(t->t_stack + SYS_KSTACK_SIZE) - FRAME_WORDS;
    memset(frame, 0, FRAME_WORDS * sizeof(frame[0]));

    frame[SLOT_RA] = (uint64_t)(uintptr_t)md_thread_start;
    frame[SLOT_S0] = (uint64_t)(uintptr_t)entry;
    frame[SLOT_S1] = (uint64_t)(uintptr_t)arg;

    t->t_sp = frame;
}
