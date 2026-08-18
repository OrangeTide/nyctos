/*
 * SPDX-License-Identifier: 0BSD OR Apache-2.0
 * Copyright (C) 2026 Jon Mayo <jon@rm-f.net>
 */

/*
 * EL1 exception vectors. Only one entry does real work: a synchronous
 * exception from a lower exception level, which is how an SVC from a
 * userland program arrives. Everything else is fatal.
 */

.macro VECTOR_ENTRY target
    .balign 0x80
    b       \target
.endm

.section ".text"
.balign 0x800
.global vectors
vectors:
    /* Current EL, SP_EL0. */
    VECTOR_ENTRY el1_fatal
    VECTOR_ENTRY el1_fatal
    VECTOR_ENTRY el1_fatal
    VECTOR_ENTRY el1_fatal

    /* Current EL, SP_ELx. */
    VECTOR_ENTRY el1_fatal
    VECTOR_ENTRY el1_fatal
    VECTOR_ENTRY el1_fatal
    VECTOR_ENTRY el1_fatal

    /* Lower EL, AArch64. */
    VECTOR_ENTRY el0_sync
    VECTOR_ENTRY el1_fatal
    VECTOR_ENTRY el1_fatal
    VECTOR_ENTRY el1_fatal

    /* Lower EL, AArch32. */
    VECTOR_ENTRY el1_fatal
    VECTOR_ENTRY el1_fatal
    VECTOR_ENTRY el1_fatal
    VECTOR_ENTRY el1_fatal

/*
 * The system call convention: x8 holds the call number, x0-x2 the
 * arguments, x0 the result. Only the caller-saved registers need saving,
 * because the C handler preserves the rest.
 */
.balign 16
el0_sync:
    sub     sp, sp, #176
    stp     x0, x1, [sp, #0]
    stp     x2, x3, [sp, #16]
    stp     x4, x5, [sp, #32]
    stp     x6, x7, [sp, #48]
    stp     x8, x9, [sp, #64]
    stp     x10, x11, [sp, #80]
    stp     x12, x13, [sp, #96]
    stp     x14, x15, [sp, #112]
    stp     x16, x17, [sp, #128]
    stp     x18, x29, [sp, #144]
    str     x30, [sp, #160]

    mrs     x9, esr_el1
    lsr     x9, x9, #26
    cmp     x9, #0x15               /* SVC from AArch64 */
    b.ne    el0_fatal

    mov     x3, x2
    mov     x2, x1
    mov     x1, x0
    mov     x0, x8
    bl      syscall_handler
    str     x0, [sp, #0]

    ldp     x0, x1, [sp, #0]
    ldp     x2, x3, [sp, #16]
    ldp     x4, x5, [sp, #32]
    ldp     x6, x7, [sp, #48]
    ldp     x8, x9, [sp, #64]
    ldp     x10, x11, [sp, #80]
    ldp     x12, x13, [sp, #96]
    ldp     x14, x15, [sp, #112]
    ldp     x16, x17, [sp, #128]
    ldp     x18, x29, [sp, #144]
    ldr     x30, [sp, #160]
    add     sp, sp, #176
    eret

el0_fatal:
    mrs     x0, esr_el1
    mrs     x1, elr_el1
    bl      trap_fatal

el1_fatal:
    mrs     x0, esr_el1
    mrs     x1, elr_el1
    bl      trap_fatal
