/* syscall.S : kernel entry point for __sysop on aarch64.
 *
 * SPDX-License-Identifier: 0BSD OR Apache-2.0
 * Copyright (C) 2026 Jon Mayo <jon@rm-f.net>
 *
 * __sysop executes svc #0 and returns. The descriptor pointer and count
 * arrive in x0 and x1 and are passed to the trap unchanged. Every sys_*
 * wrapper in sys/op.c reaches the kernel through this routine.
 *
 */

.section .text
.global __sysop
__sysop:
    svc     #0
    ret
