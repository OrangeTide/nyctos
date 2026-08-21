/*
 * SPDX-License-Identifier: 0BSD OR Apache-2.0
 * Copyright (C) 2026 Jon Mayo <jon@rm-f.net>
 */

/*
 * C runtime startup. The kernel enters here with nothing set up but the
 * program counter and x0, so crt1 owns the stack, zeroes .bss (which is
 * not part of the loaded image) and turns main()'s return value into
 * exit().
 *
 * x0 holds the pointer to the startup record of section 20.4, which is
 * the only thing this program was told. It is stashed in a callee-saved
 * register across the stack and .bss work rather than reloaded, because
 * there is nowhere to reload it from: .bss is being zeroed.
 */

.section ".text.start"
.global _start
_start:
    mov     x19, x0

    ldr     x0, =_stack_top
    mov     sp, x0

    ldr     x0, =_bss_start
    ldr     x1, =_bss_end
zero_bss:
    cmp     x0, x1
    b.ge    zero_done
    str     xzr, [x0], #8
    b       zero_bss
zero_done:

    mov     x0, x19
    bl      __libc_startup

    /*
     * argc and argv in the first two registers, fetched one at a time
     * so that no assembly here has to know the shape of a C type. The
     * call is made from assembly rather than through a C declaration,
     * which is what lets a program define main(void) and this pass
     * arguments to it anyway.
     */
    bl      __libc_argv
    mov     x20, x0
    bl      __libc_argc
    mov     x1, x20

    bl      main
    bl      exit
hang:
    b       hang
