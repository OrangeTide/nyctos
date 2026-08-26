/*
 * SPDX-License-Identifier: 0BSD OR Apache-2.0
 *
 * PUBLIC DOMAIN (CC0-1.0)
 */

/*
 * A cohort member that dies, so that what happens to the rest of the
 * cohort can be seen happening.
 *
 * It is a member of the same cohort as peer_a and peer_b, holds
 * nothing, and is not started by the recorded session. Running it stops
 * the machine, which is the whole point: section 18.2 makes the cohort
 * the recovery unit, tearing one down and restarting it is M7's work,
 * and until that exists the honest thing for a member's fault to do is
 * halt and say what is missing.
 *
 * It traps rather than touching a bad address. An address that faults
 * is a Tier 2 idea: at Tier 0 nothing is protected and reading the
 * kernel succeeds, which is what the fault checks of M4 record. An
 * illegal instruction is a fault at both tiers and needs no MMU to
 * mean anything, so this dies the same way in all four configurations.
 */

int
main(void)
{
    __builtin_trap();

    return 0;
}
