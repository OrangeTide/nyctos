/* syspage.h : layout of the read-only system page.
 *
 * SPDX-License-Identifier: 0BSD OR Apache-2.0
 * Copyright (C) 2026 Jon Mayo <jon@rm-f.net>
 *
 * Declares SYS_SYSPAGE_VERSION and sys_syspage_t, the structure of the
 * read-only page that every process maps. It holds the version and the
 * frequency of the counter a program reads for itself. Included by
 * include/os/sys.h, lib/libc/sys/uptime.c, bin/tick/tick.c, sys/kern/kern_time.c
 * and sys/kern/init_main.c.
 *
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
