/*
 * SPDX-License-Identifier: 0BSD OR Apache-2.0
 * Copyright (C) 2026 Jon Mayo <jon@rm-f.net>
 */

/*
 * Machine-mode trap entry. The only trap the kernel expects is an
 * environment call from U-mode, which is how a userland program makes a
 * system call. mscratch holds the kernel stack pointer while userland
 * runs, so the trap can swap onto it without trusting the user's sp.
 */

.section .text
.balign 4
.global trap_entry
trap_entry:
    /*
     * No stack swap. Every trap taken so far comes from a kernel thread
     * and belongs on the stack that thread is already using, which is also
     * what lets the handler switch threads: the frame stays where it will
     * be when something switches back.
     *
     * The swap through mscratch that a user trap needs, and the matching
     * one on the way out, return in stage 9 along with user mode. Doing it
     * now would mean writing a two-path exit for a case that cannot happen.
     */
    addi    sp, sp, -144
    sd      ra, 0(sp)
    sd      t0, 8(sp)
    sd      t1, 16(sp)
    sd      t2, 24(sp)
    sd      t3, 32(sp)
    sd      t4, 40(sp)
    sd      t5, 48(sp)
    sd      t6, 56(sp)
    sd      a0, 64(sp)
    sd      a1, 72(sp)
    sd      a2, 80(sp)
    sd      a3, 88(sp)
    sd      a4, 96(sp)
    sd      a5, 104(sp)
    sd      a6, 112(sp)
    sd      a7, 120(sp)

    /*
     * The return state goes in the frame, not left in the CSRs. A handler
     * may switch threads, and the thread it switches to may take a trap of
     * its own, which would overwrite mepc and mstatus before this one
     * returns.
     */
    csrr    t0, mepc
    sd      t0, 128(sp)
    csrr    t0, mstatus
    sd      t0, 136(sp)

    /*
     * An interrupt has the top bit of mcause set. Everything else is a
     * synchronous trap, of which the only expected one is a system call.
     */
    csrr    t0, mcause
    bltz    t0, trap_interrupt

    li      t1, 8                   /* environment call from U-mode */
    bne     t0, t1, trap_bad

    /*
     * Section 23.1 register mapping for this port: a0 carries the pointer
     * to the operation descriptor and a1 the count, which is where the C
     * calling convention already wants them. Nothing else is passed.
     */
    call    syscall_handler
    sd      a0, 64(sp)

    /* Resume after the ecall instruction. */
    ld      t0, 128(sp)
    addi    t0, t0, 4
    sd      t0, 128(sp)

trap_restore:
    ld      t0, 128(sp)
    csrw    mepc, t0
    ld      t0, 136(sp)
    csrw    mstatus, t0

    ld      ra, 0(sp)
    ld      t0, 8(sp)
    ld      t1, 16(sp)
    ld      t2, 24(sp)
    ld      t3, 32(sp)
    ld      t4, 40(sp)
    ld      t5, 48(sp)
    ld      t6, 56(sp)
    ld      a0, 64(sp)
    ld      a1, 72(sp)
    ld      a2, 80(sp)
    ld      a3, 88(sp)
    ld      a4, 96(sp)
    ld      a5, 104(sp)
    ld      a6, 112(sp)
    ld      a7, 120(sp)
    addi    sp, sp, 144
    mret

/*
 * An interrupt. The frame is already on the interrupted thread stack, which
 * is what makes it safe for the handler to switch threads: the context is
 * saved where it will still be when something switches back to it.
 *
 * mepc is not advanced, because an interrupt is not an instruction that
 * has been executed.
 */
trap_interrupt:
    slli    t0, t0, 1               /* drop the interrupt bit */
    srli    t0, t0, 1
    la      t1, md_pending_irq
    sd      t0, 0(t1)

    call    irq_dispatch
    j       trap_restore

trap_bad:
    csrr    a0, mcause
    csrr    a1, mepc
    call    trap_fatal
