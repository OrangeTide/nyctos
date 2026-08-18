/*
 * SPDX-License-Identifier: 0BSD OR Apache-2.0
 * Copyright (C) 2026 Jon Mayo <jon@rm-f.net>
 */

/*
 * void enter_user(unsigned long entry)
 *
 * Drops to EL0 at the given address and never returns. The MMU is off, so
 * no address space has to be built; the only thing that changes is the
 * exception level, which is enough to make SVC the sole way back into the
 * kernel. crt1 establishes the user stack (SP_EL0) itself.
 */

.section ".text"
.global enter_user
enter_user:
    msr     elr_el1, x0
    mov     x1, #0x3c0              /* EL0t, DAIF masked */
    msr     spsr_el1, x1
    isb
    eret
