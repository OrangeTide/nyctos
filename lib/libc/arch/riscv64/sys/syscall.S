/*
 * SPDX-License-Identifier: 0BSD OR Apache-2.0
 * Copyright (C) 2026 Jon Mayo <jon@rm-f.net>
 */

/*
 * long __syscall(long num, long a0, long a1, long a2)
 *
 * The kernel expects the call number in a7 and the arguments in a0-a2, so
 * every argument shifts down one register before the trap.
 */

.section .text
.global __syscall
__syscall:
    mv      a7, a0
    mv      a0, a1
    mv      a1, a2
    mv      a2, a3
    ecall
    ret

/*
 * long __sysop(sys_op_t *ops, unsigned long count)
 *
 * The operation entry point of section 23.1. Nothing shifts: a0 already
 * holds the descriptor pointer and a1 the count, which is where the trap
 * path expects them and where the C calling convention already put them.
 */
.global __sysop
__sysop:
    ecall
    ret
