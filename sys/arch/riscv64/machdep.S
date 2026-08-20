/*
 * SPDX-License-Identifier: 0BSD OR Apache-2.0
 * Copyright (C) 2026 Jon Mayo <jon@rm-f.net>
 */

/*
 * void enter_user(unsigned long entry, const sys_startup_t *startup)
 *
 * Drops from M-mode to U-mode at the given address and never returns. A
 * single PMP entry covering the whole address space is needed because
 * without one U-mode may access nothing at all. crt1 establishes the user
 * stack itself, so only the kernel's stack pointer is stashed here, in
 * mscratch, which is the invariant trap_entry reads to tell a user trap
 * from a kernel one.
 *
 * The startup record of section 20.4 arrives in a0, which is where crt1
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
    mret
