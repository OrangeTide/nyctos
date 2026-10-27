/* syscall.S : kernel entry point for __sysop on riscv64.
 *
 * SPDX-License-Identifier: 0BSD OR Apache-2.0
 * Copyright (C) 2026 Jon Mayo <jon@rm-f.net>
 *
 * __sysop executes ecall and returns. The descriptor pointer and count
 * arrive in a0 and a1 and are passed to the trap unchanged. Every sys_*
 * wrapper in sys/op.c reaches the kernel through this routine.
 *
 */

.section .text
.global __sysop
__sysop:
    ecall
    ret
