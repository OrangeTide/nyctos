/*
 * PUBLIC DOMAIN (CC0-1.0)
 */

/*
 * The clock, read without a system call, per section 6.2.
 *
 * One instruction on each port, and the same instruction the kernel
 * tells the time with: rdtime on riscv64 and cntpct_el0 on aarch64. A
 * program reading a different counter would be reading a clock that
 * agrees with the kernel's only by accident, and a deadline is a number
 * both of them have to mean the same thing by.
 *
 * The port has to permit it, once, at boot: scounteren bit TM and
 * CNTKCTL_EL1 bit EL0PCTEN. Without that these are not slow, they are
 * illegal, and a program taking the time takes a fault instead.
 *
 * What comes back is ticks and not seconds. How long a tick is belongs
 * to the machine, and the system page is where a program learns it,
 * which is what sys_uptime_hz is for. Section 6.2 calls this
 * sys_syspage_uptime() because it expected the page to carry a stored
 * value; stage one of milestone 16 measured what such a value would be
 * worth in a tickless kernel and the answer was up to 141 milliseconds
 * stale. So the page carries the frequency, the counter carries the
 * time, and the name here says which of them this reads.
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
 * A deadline, from now, in ticks. Section 14.3 names this
 * sys_deadline_in() and puts it in libc rather than in the kernel: the
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
 * program needs both halves of section 6.2: the counter for the instant
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
