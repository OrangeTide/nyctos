/*
 * SPDX-License-Identifier: 0BSD OR Apache-2.0
 * Copyright (C) 2026 Jon Mayo <jon@rm-f.net>
 */

/*
 * Standard output, over the debug backdoor of section 22.4.
 *
 * There are no file descriptors, because there is no console service to
 * hold a connection to and no descriptor table to keep one in. A write to
 * standard output or standard error therefore goes to the kernel console
 * and everything else is refused, which is a truthful account of what this
 * system can currently do rather than a stub that pretends otherwise.
 *
 * When a console service exists, this becomes an ordinary message to a
 * handle the process was given at spawn, and the backdoor goes.
 */

#include <os/sys.h>
#include <unistd.h>

ssize_t
write(int fd, const void *buf, size_t nbytes)
{
    size_t done = 0;

    if (fd != STDOUT_FILENO && fd != STDERR_FILENO) {
        return -1;
    }

    /*
     * Split at the kernel's bound rather than failing above it. Each call
     * is a preemption point, which is why the bound is there.
     */
    while (done < nbytes) {
        size_t n = nbytes - done;

        if (n > SYS_DEBUG_WRITE_MAX) {
            n = SYS_DEBUG_WRITE_MAX;
        }
        if (sys_debug_write((const char *)buf + done, (uint32_t)n) != SYS_OK) {
            return done == 0 ? -1 : (ssize_t)done;
        }
        done += n;
    }

    return (ssize_t)done;
}
