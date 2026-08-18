/*
 * SPDX-License-Identifier: 0BSD OR Apache-2.0
 * Copyright (C) 2026 Jon Mayo <jon@rm-f.net>
 */

/*
 * C runtime startup. The kernel enters here with nothing set up but the
 * program counter, so crt1 owns the stack, zeroes .bss (which is not part
 * of the loaded image) and turns main()'s return value into exit().
 */

.section .text.start
.global _start
_start:
    la      sp, _stack_top

    la      t0, _bss_start
    la      t1, _bss_end
zero_bss:
    bge     t0, t1, zero_done
    sd      zero, 0(t0)
    addi    t0, t0, 8
    j       zero_bss
zero_done:

    call    main
    call    exit
hang:
    j       hang
