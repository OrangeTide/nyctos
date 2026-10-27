/* uptime.c : clock and deadline helpers for programs.
 *
 * SPDX-License-Identifier: 0BSD OR Apache-2.0
 * Copyright (C) 2026 Jon Mayo <jon@rm-f.net>
 *
 * sys_uptime reads the hardware counter (rdtime on riscv64, cntpct_el0 on
 * aarch64) and returns ticks. sys_uptime_hz returns the tick frequency from
 * the system page, or 0 if the page is missing or has another version.
 * sys_deadline_in and sys_deadline_in_ms return an absolute deadline in ticks
 * or milliseconds from now, and sys_deadline_in_ms returns SYS_DEADLINE_NOW
 * when the frequency is 0.
 *
 */

#include <os/sys.h>
#include <sys/syspage.h>

uint64_t
sys_uptime(void)
{
    uint64_t v;

#if defined(__riscv)
    __asm__ volatile ("rdtime %0" : "=r" (v));
#elif defined(__aarch64__)
    /*
     * The barrier for the reason the kernel's read has one: the counter
     * read is not ordered against the instructions around it, and two
     * reads either side of some work can otherwise come back in the
     * wrong order.
     */
    __asm__ volatile ("isb");
    __asm__ volatile ("mrs %0, cntpct_el0" : "=r" (v));
#else
#error "no way to read the clock on this port"
#endif

    return v;
}

uint64_t
sys_uptime_hz(void)
{
    const sys_syspage_t *sp = sys_syspage();

    /*
     * Zero for a process with no page, which is a process that cannot
     * turn ticks into a duration. It is an answer rather than a guess:
     * a wrong frequency makes every deadline built from it wrong by
     * the same factor, silently.
     */
    if (sp == NULL || sp->version != SYS_SYSPAGE_VERSION) {
        return 0;
    }

    return sp->hz;
}

/*
 * A deadline, from now, in ticks. This is
 * sys_deadline_in(), which is in libc rather than in the kernel: the
 * kernel takes an absolute instant on the monotonic timebase and has no
 * business converting one, because a relative deadline converted inside
 * a system call would start counting from whenever the call got there.
 */
uint64_t
sys_deadline_in(uint64_t ticks)
{
    return sys_uptime() + ticks;
}

/*
 * The same in milliseconds, which is what a program has in mind.
 *
 * The frequency comes from the system page, so this is the one place a
 * program needs both the counter for the instant
 * and the page for the scale. Without the page there is no scale, and
 * the answer is SYS_DEADLINE_NOW: a call given it returns at once
 * rather than waiting, which is a program failing where it can be seen
 * instead of waiting for a length of time nobody chose.
 */
uint64_t
sys_deadline_in_ms(uint32_t ms)
{
    uint64_t hz = sys_uptime_hz();

    if (hz == 0) {
        return SYS_DEADLINE_NOW;
    }

    return sys_deadline_in((uint64_t)ms * hz / 1000u);
}
