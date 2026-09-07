/*
 * PUBLIC DOMAIN (CC0-1.0)
 */

/*
 * The system page of section 6.2: one page of kernel memory, mapped
 * read-only into every process, that a program reads without a syscall.
 *
 * What it carries is what a program cannot obtain any other way and
 * cannot be told once at startup because it changes. Today that is the
 * timebase: section 14.3 has every blocking call take an absolute
 * deadline on the monotonic clock, and a program with no way to read
 * that clock cannot construct one.
 *
 * It does not carry the time. Section 6.2 describes a 64-bit
 * uptime_ticks field the kernel writes and a program reads under a
 * seqlock, and M16 stage one measured what that would be worth in a
 * kernel that arms the hardware for the next event and for nothing
 * else: the longest stretch with no kernel running in it is about a
 * tenth of a second, and what runs across it is a program computing.
 * A deadline built from a value that old expires before it is made.
 *
 * So the page carries the frequency and the program reads the counter,
 * which both ports have as an instruction and neither permitted from
 * user mode until this milestone. The seqlock has nothing to protect,
 * because there is no field the kernel writes after boot.
 */

#ifndef _SYS_SYSPAGE_H_
#define _SYS_SYSPAGE_H_

#include <stdint.h>

#define SYS_SYSPAGE_VERSION     1u

typedef struct sys_syspage {
    uint32_t    version;
    uint32_t    _reserved;

    /*
     * Ticks per second of the counter a program reads for itself. A
     * program turns a duration into a deadline with this and nothing
     * else, so it is the whole of what section 14.3 needs from here.
     */
    uint64_t    hz;
} sys_syspage_t;

#endif /* _SYS_SYSPAGE_H_ */
