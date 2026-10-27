/* machdep.S : RISC-V transition from supervisor to user mode.
 *
 * SPDX-License-Identifier: 0BSD OR Apache-2.0
 * Copyright (C) 2026 Jon Mayo <jon@rm-f.net>
 *
 * enter_user saves tp below the kernel stack, clears tp for the program,
 * and stores the kernel stack pointer in sscratch for trap_entry. It then
 * sets SPP to user mode and SPIE, sets sepc to the entry address, and
 * executes sret into the program with the argument in a0. A nonzero sp
 * is installed as the user stack. The function does not return.
 *
 */

.section .text
.global enter_user
enter_user:
    /*
     * This core's own pointer parked below the kernel stack, where the
     * trap path finds it, and the register handed to the program
     * cleared. A program starts with no thread pointer rather than
     * with a kernel address in the register the kernel keeps its
     * per-core state in.
     */
    sd      tp, -8(sp)
    mv      tp, zero

    csrw    sscratch, sp

    li      t0, (1 << 8)           /* sstatus.SPP = 0: return to U-mode */
    csrc    sstatus, t0

    li      t0, (1 << 5)            /* sstatus.SPIE: interrupts on return */
    csrs    sstatus, t0

    csrw    sepc, a0
    mv      a0, a1

    /*
     * Last, because sp is the stack this code is running on: everything
     * above needs the kernel stack and nothing below touches memory.
     */
    beqz    a2, 1f
    mv      sp, a2
1:
    sret
