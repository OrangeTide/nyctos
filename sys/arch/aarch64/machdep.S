/*
 * SPDX-License-Identifier: 0BSD OR Apache-2.0
 * Copyright (C) 2026 Jon Mayo <jon@rm-f.net>
 */

/*
 * void enter_user(unsigned long entry, void *arg, unsigned long sp)
 *
 * Drops to EL0 at the given address and never returns. The MMU is off, so
 * no address space has to be built; the only thing that changes is the
 * exception level, which is enough to make SVC the sole way back into the
 * kernel.
 *
 * arg arrives in x0. For a process's first thread that is the startup
 * record of section 20.4, which is the only thing the program is told; for
 * a thread the program created it is whatever the creator passed.
 *
 * sp is the user stack, or zero to leave SP_EL0 alone. A program's first
 * thread sets its own stack in crt1, because the image is where the stack
 * is and the kernel does not know the image. A thread the program created
 * runs on a stack the creator supplied, per section 10.2, and the kernel
 * has to install it because the thread has no code that runs first.
 */

.section ".text"
.global enter_user
enter_user:
    msr     elr_el1, x0

    cbz     x2, 1f
    msr     sp_el0, x2
1:

    /*
     * EL0t with IRQ unmasked. A program that cannot be interrupted is
     * a program no device can be waited for behind, and the lower-EL
     * IRQ vector is what makes this safe. Debug, SError and FIQ stay
     * masked: nothing in this system raises any of them.
     */
    mov     x2, #0x340              /* EL0t, IRQ unmasked */
    msr     spsr_el1, x2
    mov     x0, x1
    isb
    eret
