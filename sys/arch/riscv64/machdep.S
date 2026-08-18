/*
 * SPDX-License-Identifier: 0BSD OR Apache-2.0
 * Copyright (C) 2026 Jon Mayo <jon@rm-f.net>
 */

/*
 * void enter_user(unsigned long entry)
 *
 * Drops from M-mode to U-mode at the given address and never returns. A
 * single PMP entry covering the whole address space is needed because
 * without one U-mode may access nothing at all. crt1 establishes the user
 * stack itself, so only the kernel's stack pointer is stashed here, in
 * mscratch, for the trap handler to swap back to.
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

    csrw    mepc, a0
    mret
