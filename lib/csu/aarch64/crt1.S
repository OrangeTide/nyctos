/*
 * SPDX-License-Identifier: 0BSD OR Apache-2.0
 * Copyright (C) 2026 Jon Mayo <jon@rm-f.net>
 */

/*
 * C runtime startup. The kernel enters here with nothing set up but the
 * program counter, so crt1 owns the stack, zeroes .bss (which is not part
 * of the loaded image) and turns main()'s return value into exit().
 */

.section ".text.start"
.global _start
_start:
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

    bl      main
    bl      exit
hang:
    b       hang
