/* machdep.S : AArch64 transition from EL1 to EL0.
 *
 * SPDX-License-Identifier: 0BSD OR Apache-2.0
 * Copyright (C) 2026 Jon Mayo <jon@rm-f.net>
 *
 * enter_user sets ELR_EL1 to the entry address, installs the user stack
 * in SP_EL0 when one is given, and executes eret to EL0t with IRQs
 * unmasked. The argument is passed to the program in x0. Debug, SError,
 * and FIQ stay masked. The function does not return.
 *
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
