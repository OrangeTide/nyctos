/*
 * SPDX-License-Identifier: 0BSD OR Apache-2.0
 * Copyright (C) 2026 Jon Mayo <jon@rm-f.net>
 */

#include <sys/config.h>

#define SECONDARY_STACK_BYTES   4096

.section ".text.boot"
.global _start

_start:
    /*
     * Only core zero arrives here at reset. The platform holds the
     * others powered off until asked for one by name, which is the
     * opposite of the other port, where every hart arrives together
     * and has to be told to wait.
     */
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

/*
 * Where a core the platform has just started arrives.
 *
 * It has nothing: no stack, no vector table, and whatever exception
 * level the firmware left it at. All three have to be arranged before it
 * can enter C, which is why the entry point the kernel asks for is not
 * the one the platform is given.
 */
.global secondary_entry
secondary_entry:
    mrs     x0, CurrentEL
    lsr     x0, x0, #2
    cmp     x0, #2
    b.ne    secondary_at_el1
    msr     sctlr_el1, xzr
    movz    x0, #0x8000, lsl #16    /* HCR_EL2.RW: EL1 is AArch64 */
    msr     hcr_el2, x0
    mov     x0, #0x3c5              /* EL1h, DAIF masked */
    msr     spsr_el2, x0
    adr     x0, secondary_at_el1
    msr     elr_el2, x0
    eret

secondary_at_el1:
    /*
     * Its own stack, indexed by which core it is. The .bss this reads
     * was zeroed by the core that booted, long before this one was
     * asked for.
     */
    mrs     x0, mpidr_el1
    and     x0, x0, #3

    /*
     * A core the build has no room for parks here and touches nothing.
     * The stacks below are indexed by which core this is, and a core
     * numbered past the end of them would take its stack from whatever
     * the linker put next.
     */
    cmp     x0, #SYS_MAX_CORES
    b.lo    1f
0:  wfi
    b       0b
1:
    ldr     x1, =secondary_stacks
    add     x0, x0, #1
    mov     x2, #SECONDARY_STACK_BYTES
    madd    x1, x0, x2, x1
    mov     sp, x1

    ldr     x0, =vectors
    msr     vbar_el1, x0
    isb

    bl      cpu_secondary_entry
    b       hang

.section .bss
.balign 16
secondary_stacks:
    .space  SECONDARY_STACK_BYTES * SYS_MAX_CORES
