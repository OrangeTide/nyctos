/*
 * SPDX-License-Identifier: 0BSD OR Apache-2.0
 * Copyright (C) 2026 Jon Mayo <jon@rm-f.net>
 */

/*
 * long __syscall(long num, long a0, long a1, long a2)
 *
 * The kernel expects the call number in x8 and the arguments in x0-x2, so
 * every argument shifts down one register before the trap.
 */

.section .text
.global __syscall
__syscall:
    mov     x8, x0
    mov     x0, x1
    mov     x1, x2
    mov     x2, x3
    svc     #0
    ret
