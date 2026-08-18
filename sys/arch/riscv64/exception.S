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
    csrrw   sp, mscratch, sp

    addi    sp, sp, -128
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

    csrr    t0, mcause
    li      t1, 8                   /* environment call from U-mode */
    bne     t0, t1, trap_bad

    /* a7 holds the call number, a0-a2 the arguments. */
    mv      a3, a2
    mv      a2, a1
    mv      a1, a0
    mv      a0, a7
    call    syscall_handler
    sd      a0, 64(sp)

    /* Resume after the ecall instruction. */
    csrr    t0, mepc
    addi    t0, t0, 4
    csrw    mepc, t0

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
    addi    sp, sp, 128

    csrrw   sp, mscratch, sp
    mret

trap_bad:
    csrr    a0, mcause
    csrr    a1, mepc
    call    trap_fatal
