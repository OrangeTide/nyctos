/*
 * SPDX-License-Identifier: 0BSD OR Apache-2.0
 * Copyright (C) 2026 Jon Mayo <jon@rm-f.net>
 */

/*
 * Machine-mode trap entry.
 *
 * mscratch carries the whole of the kernel-versus-user distinction on this
 * port: it holds the running thread's kernel stack pointer while that
 * thread is in user mode, and zero while it is in the kernel. A trap can
 * therefore find its stack without trusting the user's sp and without
 * needing a spare register to decide which case it is in, because the swap
 * itself answers the question.
 */

.section .text
.balign 4
.global trap_entry
trap_entry:
    /*
     * Swap unconditionally, then look at what came back. A nonzero value
     * was a parked kernel stack pointer, so this trap came from user mode
     * and sp now points at the right stack. Zero means the kernel was
     * already running on its own stack, so the swap is undone and nothing
     * has moved.
     */
    csrrw   sp, mscratch, sp
    bnez    sp, 1f
    csrrw   sp, mscratch, sp
1:
    /*
     * The frame stays on the stack of the thread that took the trap, which
     * is what lets the handler switch threads: the context is saved where
     * it will still be when something switches back to it.
     */
    addi    sp, sp, -160
    sd      ra, 0(sp)
    sd      t0, 8(sp)

    /*
     * mscratch now holds the user stack pointer, or zero for a trap that
     * came from the kernel. Either way it belongs in the frame rather than
     * in a register, for the same reason mepc does: a thread switch would
     * otherwise lose it. Clearing it restores the in-kernel invariant.
     */
    csrr    t0, mscratch
    sd      t0, 144(sp)
    csrw    mscratch, zero

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

    /*
     * MPP in the saved mstatus, still in t0, says where mret will land.
     * Zero is user mode, and only that case has a stack to swap back to.
     */
    srli    t0, t0, 11
    andi    t0, t0, 3
    bnez    t0, trap_restore_kernel

    /*
     * Park the user stack pointer in mscratch, restore the last register,
     * then swap: sp becomes the user stack and mscratch is left holding
     * this thread's kernel stack for the next trap. That is the invariant
     * the entry path above reads.
     */
    ld      t0, 144(sp)
    csrw    mscratch, t0
    ld      t0, 8(sp)
    addi    sp, sp, 160
    csrrw   sp, mscratch, sp
    mret

trap_restore_kernel:
    ld      t0, 8(sp)
    addi    sp, sp, 160
    mret

/*
 * An interrupt. mepc is not advanced, because an interrupt is not an
 * instruction that has been executed.
 */
trap_interrupt:
    slli    t0, t0, 1               /* drop the interrupt bit */
    srli    t0, t0, 1
    la      t1, md_pending_irq
    sd      t0, 0(t1)

    /*
     * MPP in the saved mstatus says where this trap came from, and zero
     * is user mode. Nothing above the trap path can tell the two apart,
     * and a program being interruptible at all is new.
     */
    ld      t1, 136(sp)
    srli    t1, t1, 11
    andi    t1, t1, 3
    bnez    t1, 2f
    la      t1, md_user_irqs
    ld      t2, 0(t1)
    addi    t2, t2, 1
    sd      t2, 0(t1)
2:

    call    irq_dispatch
    j       trap_restore

trap_bad:
    csrr    a0, mcause
    csrr    a1, mepc
    call    trap_fatal

.section .data
.balign 8
.global md_user_irqs
md_user_irqs:
    .dword 0
