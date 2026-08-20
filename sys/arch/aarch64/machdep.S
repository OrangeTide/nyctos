/*
 * SPDX-License-Identifier: 0BSD OR Apache-2.0
 * Copyright (C) 2026 Jon Mayo <jon@rm-f.net>
 */

/*
 * void enter_user(unsigned long entry, const sys_startup_t *startup)
 *
 * Drops to EL0 at the given address and never returns. The MMU is off, so
 * no address space has to be built; the only thing that changes is the
 * exception level, which is enough to make SVC the sole way back into the
 * kernel. crt1 establishes the user stack (SP_EL0) itself.
 *
 * The startup record of section 20.4 arrives in x0, which is where crt1
 * expects it. It is the only thing the program is told.
 *
 * DAIF stays masked, so a program cannot be interrupted. The lower-EL IRQ
 * vector is fatal, and unmasking here without wiring that vector would
 * turn the first timer tick into a panic.
 */

.section ".text"
.global enter_user
enter_user:
    msr     elr_el1, x0
    mov     x2, #0x3c0              /* EL0t, DAIF masked */
    msr     spsr_el1, x2
    mov     x0, x1
    isb
    eret
