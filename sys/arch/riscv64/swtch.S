/*
 * SPDX-License-Identifier: 0BSD OR Apache-2.0
 * Copyright (C) 2026 Jon Mayo <jon@rm-f.net>
 */

/*
 * void md_switch(void **old_sp, void *new_sp)
 *
 * Saves the callee-saved state of the calling thread onto its own stack,
 * records the resulting stack pointer through old_sp, then adopts new_sp
 * and restores from it. Returns in the thread that new_sp belongs to.
 *
 * ra is saved with the rest because it is where the switch returns to, and
 * a thread that has never run has a forged one. The frame is 112 bytes
 * rather than the 104 the registers need, to keep the 16-byte alignment
 * the ABI asks for. The layout must agree with md_thread_init in
 * thread_md.c.
 */

.section .text
.global md_switch
md_switch:
    addi    sp, sp, -112
    sd      ra, 0(sp)
    sd      s0, 8(sp)
    sd      s1, 16(sp)
    sd      s2, 24(sp)
    sd      s3, 32(sp)
    sd      s4, 40(sp)
    sd      s5, 48(sp)
    sd      s6, 56(sp)
    sd      s7, 64(sp)
    sd      s8, 72(sp)
    sd      s9, 80(sp)
    sd      s10, 88(sp)
    sd      s11, 96(sp)

    sd      sp, 0(a0)
    mv      sp, a1

    ld      ra, 0(sp)
    ld      s0, 8(sp)
    ld      s1, 16(sp)
    ld      s2, 24(sp)
    ld      s3, 32(sp)
    ld      s4, 40(sp)
    ld      s5, 48(sp)
    ld      s6, 56(sp)
    ld      s7, 64(sp)
    ld      s8, 72(sp)
    ld      s9, 80(sp)
    ld      s10, 88(sp)
    ld      s11, 96(sp)
    addi    sp, sp, 112
    ret

/*
 * Where a new thread begins. md_switch restored s0 and s1 from the frame
 * md_thread_init planted, so the entry point and its argument are already
 * in hand and there is nothing on the stack to return to.
 */
.global md_thread_start
md_thread_start:
    /*
     * Every switch happens inside an interrupts-disabled region, so a
     * thread that has never run inherits that state and has to leave it
     * itself. A thread that started with interrupts masked would never be
     * preempted and never see a timer.
     */
    csrsi   sstatus, 2              /* sstatus.SIE */
    mv      a0, s1
    jalr    s0
    call    thread_exited
