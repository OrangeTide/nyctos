/*
 * SPDX-License-Identifier: 0BSD OR Apache-2.0
 * Copyright (C) 2026 Jon Mayo <jon@rm-f.net>
 */

/*
 * void enter_user(unsigned long entry, void *arg, unsigned long sp)
 *
 * Drops from M-mode to U-mode at the given address and never returns. A
 * single PMP entry covering the whole address space is needed because
 * without one U-mode may access nothing at all. The kernel stack pointer
 * is stashed in mscratch, which is the invariant trap_entry reads to tell
 * a user trap from a kernel one.
 *
 * arg arrives in a0. For a process's first thread that is the startup
 * record of section 20.4; for a thread the program created it is whatever
 * the creator passed.
 *
 * sp is the user stack, or zero to leave it as it is. A program's first
 * thread sets its own stack in crt1, because the image is where the stack
 * is and the kernel does not know the image. A thread the program created
 * runs on a stack the creator supplied, per section 10.2, and the kernel
 * has to install it because the thread has no code that runs first.
 * expects it. It is the only thing the program is told.
 *
 * A program runs with machine interrupts live. U-mode cannot mask them
 * and never could: the privileged specification enables interrupts from
 * a more privileged mode regardless of that mode's own enable bit, so
 * what actually decides is mie, and what changed is that the AArch64
 * side now has a lower-EL vector to take them on. MPIE is set here so
 * that the state a program returns to is the state the kernel meant
 * rather than whatever was left over.
 */

.section .text
.global enter_user
enter_user:
    csrw    mscratch, sp

    li      t0, -1
    srli    t0, t0, 10              /* NAPOT range covering all of memory */
    csrw    pmpaddr0, t0
    li      t0, 0x1f                /* NAPOT, read/write/execute */
    csrw    pmpcfg0, t0

    li      t0, (3 << 11)           /* mstatus.MPP = 0: return to U-mode */
    csrc    mstatus, t0

    li      t0, (1 << 7)            /* mstatus.MPIE: interrupts on return */
    csrs    mstatus, t0

    csrw    mepc, a0
    mv      a0, a1

    /*
     * Last, because sp is the stack this code is running on: everything
     * above needs the kernel stack and nothing below touches memory.
     */
    beqz    a2, 1f
    mv      sp, a2
1:
    mret
