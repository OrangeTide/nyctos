/*
 * SPDX-License-Identifier: 0BSD OR Apache-2.0
 * Copyright (C) 2026 Jon Mayo <jon@rm-f.net>
 */

/*
 * long __sysop(sys_op_t *ops, unsigned long count)
 *
 * The operation entry point of section 23.1, and the only way into the
 * kernel. Nothing shifts: x0 already holds the descriptor pointer and x1
 * the count, which is where the trap path expects them and where the C
 * calling convention already put them.
 */

.section .text
.global __sysop
__sysop:
    svc     #0
    ret
