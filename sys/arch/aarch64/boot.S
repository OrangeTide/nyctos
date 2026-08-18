/*
 * SPDX-License-Identifier: 0BSD OR Apache-2.0
 * Copyright (C) 2026 Jon Mayo <jon@rm-f.net>
 */

.section ".text.boot"
.global _start

_start:
    /* Park all secondary cores; only core 0 continues. */
    mrs     x0, mpidr_el1
    and     x0, x0, #3
    cbz     x0, primary_core
hang:
    wfe
    b       hang

primary_core:
    /* QEMU may enter at EL2; the kernel wants to run at EL1. */
    mrs     x0, CurrentEL
    lsr     x0, x0, #2
    cmp     x0, #2
    b.ne    at_el1
    msr     sctlr_el1, xzr
    movz    x0, #0x8000, lsl #16    /* HCR_EL2.RW: EL1 is AArch64 */
    msr     hcr_el2, x0
    mov     x0, #0x3c5              /* EL1h, DAIF masked */
    msr     spsr_el2, x0
    adr     x0, at_el1
    msr     elr_el2, x0
    eret

at_el1:
    /* Zero the .bss section before entering C code. */
    ldr     x0, =_bss_start
    ldr     x1, =_bss_end
zero_bss:
    cmp     x0, x1
    b.ge    zero_done
    str     xzr, [x0], #8
    b       zero_bss
zero_done:

    ldr     x0, =_stack_top
    mov     sp, x0

    ldr     x0, =vectors
    msr     vbar_el1, x0
    isb

    bl      kernel_main
    b       hang
