/*
 * SPDX-License-Identifier: 0BSD OR Apache-2.0
 * Copyright (C) 2026 Jon Mayo <jon@rm-f.net>
 */

.section .text.boot
.global _start

_start:
    /* Zero the .bss section before entering C code. */
    la      t0, _bss_start
    la      t1, _bss_end
zero_bss:
    bge     t0, t1, zero_done
    sd      zero, 0(t0)
    addi    t0, t0, 8
    j       zero_bss
zero_done:

    la      sp, _stack_top

    la      t0, trap_entry
    csrw    mtvec, t0

    call    kernel_main

hang:
    wfi
    j       hang
