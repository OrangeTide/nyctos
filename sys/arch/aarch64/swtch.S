/* swtch.S : AArch64 thread context switch.
 *
 * SPDX-License-Identifier: 0BSD OR Apache-2.0
 * Copyright (C) 2026 Jon Mayo <jon@rm-f.net>
 *
 * md_switch pushes x19 through x30 and the stack pointer of the outgoing
 * thread, stores that stack pointer through old_sp, then loads the same
 * registers from new_sp and returns onto the incoming thread's stack.
 * md_thread_start unmasks IRQs, calls the thread's entry point with its
 * argument in x0, and calls thread_exited if the entry point returns.
 *
 */

.section ".text"
.global md_switch
md_switch:
    sub     sp, sp, #96
    stp     x19, x20, [sp, #0]
    stp     x21, x22, [sp, #16]
    stp     x23, x24, [sp, #32]
    stp     x25, x26, [sp, #48]
    stp     x27, x28, [sp, #64]
    stp     x29, x30, [sp, #80]

    mov     x2, sp
    str     x2, [x0]
    mov     sp, x1

    ldp     x19, x20, [sp, #0]
    ldp     x21, x22, [sp, #16]
    ldp     x23, x24, [sp, #32]
    ldp     x25, x26, [sp, #48]
    ldp     x27, x28, [sp, #64]
    ldp     x29, x30, [sp, #80]
    add     sp, sp, #96
    ret

/*
 * Where a new thread begins. md_switch restored x19 and x20 from the frame
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
    msr     daifclr, #2
    mov     x0, x20
    blr     x19
    bl      thread_exited
