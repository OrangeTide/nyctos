/* peer_c.c : a cohort member that traps on purpose.
 *
 * SPDX-License-Identifier: 0BSD OR Apache-2.0
 * Copyright (C) 2026 Jon Mayo <jon@rm-f.net>
 *
 * It belongs to the same cohort as peer_a and peer_b and holds no state. Its
 * main executes a trap instruction at once, which is an illegal instruction
 * fault at every tier, so the machine halts. Nothing restarts the cohort
 * after a member faults.
 *
 */

int
main(void)
{
    __builtin_trap();

    return 0;
}
