/*
 * SPDX-License-Identifier: 0BSD OR Apache-2.0
 * Copyright (C) 2026 Jon Mayo <jon@rm-f.net>
 */

/*
 * EL1 exception vectors. Three entries do real work: a synchronous
 * exception from a lower exception level, which is how an SVC from a
 * userland program arrives, and an IRQ at either level, because a
 * program can now be interrupted as well as a kernel thread.
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

    /* Current EL, SP_ELx. This is where a kernel thread takes an IRQ. */
    VECTOR_ENTRY el1_fatal
    VECTOR_ENTRY el1_irq
    VECTOR_ENTRY el1_fatal
    VECTOR_ENTRY el1_fatal

    /* Lower EL, AArch64. */
    VECTOR_ENTRY el0_sync
    VECTOR_ENTRY el0_irq
    VECTOR_ENTRY el1_fatal
    VECTOR_ENTRY el1_fatal

    /* Lower EL, AArch32. */
    VECTOR_ENTRY el1_fatal
    VECTOR_ENTRY el1_fatal
    VECTOR_ENTRY el1_fatal
    VECTOR_ENTRY el1_fatal

/*
 * The system call convention: x0 carries the pointer to the operation
 * descriptor and x1 the count, per section 23.1, and x0 the result. Only
 * the caller-saved registers are in the frame, because the C handler
 * preserves the rest and a system call is a call.
 */
.balign 16
el0_sync:
    sub     sp, sp, #192
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

    /*
     * The return state goes in the frame rather than being left in the
     * system registers. A handler may switch threads, and the thread it
     * switches to may take an exception of its own, which would overwrite
     * elr and spsr before this one returns.
     *
     * SP_EL0 is in the frame for the same reason and only that reason.
     * EL1 runs on SP_EL1, so nothing here would disturb it within one
     * trap; a thread switch is what makes it another thread's stack.
     */
    mrs     x9, elr_el1
    mrs     x10, spsr_el1
    stp     x9, x10, [sp, #168]
    mrs     x9, sp_el0
    str     x9, [sp, #184]

    mrs     x9, esr_el1
    lsr     x9, x9, #26
    cmp     x9, #0x15               /* SVC from AArch64 */
    b.ne    el0_fatal

    bl      syscall_handler
    str     x0, [sp, #0]

    /*
     * System registers first, general registers last. The other order
     * needs two of them as scratch after they have been restored, which
     * hands the program its own elr and spsr back in place of x9 and x10.
     */
    ldr     x9, [sp, #184]
    msr     sp_el0, x9
    ldp     x9, x10, [sp, #168]
    msr     elr_el1, x9
    msr     spsr_el1, x10

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
    add     sp, sp, #192
    eret

el0_fatal:
    mrs     x0, esr_el1
    mrs     x1, elr_el1
    bl      trap_fatal

el1_fatal:
    mrs     x0, esr_el1
    mrs     x1, elr_el1
    bl      trap_fatal

/*
 * An IRQ taken while a kernel thread is running. The frame lands on that
 * thread's own stack, which is what makes it safe for the handler to switch
 * threads: the interrupted context is saved where it will still be when
 * something switches back to it.
 *
 * Only the caller-saved registers are here. irq_dispatch is a C function,
 * so the rest are its problem.
 */
.balign 16
.global el1_irq
el1_irq:
    sub     sp, sp, #192
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

    /*
     * The return state goes in the frame rather than being left in the
     * system registers. A handler may switch threads, and the thread it
     * switches to may take an exception of its own, which would overwrite
     * elr and spsr before this one returns.
     */
    mrs     x9, elr_el1
    mrs     x10, spsr_el1
    stp     x9, x10, [sp, #168]

    bl      irq_dispatch

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

    ldp     x9, x10, [sp, #168]
    msr     elr_el1, x9
    msr     spsr_el1, x10
    ldr     x30, [sp, #160]
    add     sp, sp, #192
    eret

/*
 * An IRQ taken while a userland program is running.
 *
 * The same frame as el1_irq, plus SP_EL0, and for the same reason el0_sync
 * saves it: EL1 runs on SP_EL1, so nothing within one trap disturbs the
 * user stack pointer, but a thread switch inside the handler makes it
 * another thread's. An interrupt can switch threads exactly as a system
 * call can, which is the whole reason this entry cannot simply be el1_irq.
 */
.balign 16
.global el0_irq
el0_irq:
    sub     sp, sp, #192
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

    mrs     x9, elr_el1
    mrs     x10, spsr_el1
    stp     x9, x10, [sp, #168]
    mrs     x9, sp_el0
    str     x9, [sp, #184]

    /*
     * Counted here because nothing above the trap path can tell an
     * interrupt taken in user mode from one taken in the kernel, and
     * after the frame is built because x9 and x10 belong to the program
     * until they are in it.
     */
    adrp    x9, md_user_irqs
    add     x9, x9, :lo12:md_user_irqs
    ldr     x10, [x9]
    add     x10, x10, #1
    str     x10, [x9]

    bl      irq_dispatch

    ldr     x9, [sp, #184]
    msr     sp_el0, x9
    ldp     x9, x10, [sp, #168]
    msr     elr_el1, x9
    msr     spsr_el1, x10

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
    add     sp, sp, #192
    eret

.section ".data"
.balign 8
.global md_user_irqs
md_user_irqs:
    .quad 0
