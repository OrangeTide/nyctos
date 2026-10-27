/* crt1.S : entry point for riscv64 programs.
 *
 * SPDX-License-Identifier: 0BSD OR Apache-2.0
 * Copyright (C) 2026 Jon Mayo <jon@rm-f.net>
 *
 * Entry point _start for riscv64 programs. It keeps the startup record
 * pointer from a0 in s0, sets up the stack, and zeroes .bss. It then calls
 * __libc_startup, loads argc and argv for main, and passes main's return
 * value to exit.
 *
 */

.section .text.start
.global _start
_start:
    mv      s0, a0

    la      sp, _stack_top

    la      t0, _bss_start
    la      t1, _bss_end
zero_bss:
    bge     t0, t1, zero_done
    sd      zero, 0(t0)
    addi    t0, t0, 8
    j       zero_bss
zero_done:

    mv      a0, s0
    call    __libc_startup

    /*
     * argc and argv in the first two registers, fetched one at a time
     * so that no assembly here has to know the shape of a C type. The
     * call is made from assembly rather than through a C declaration,
     * which is what lets a program define main(void) and this pass
     * arguments to it anyway.
     */
    call    __libc_argv
    mv      s1, a0
    call    __libc_argc
    mv      a1, s1


    call    main
    call    exit
hang:
    j       hang
