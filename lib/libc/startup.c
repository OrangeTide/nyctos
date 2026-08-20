/*
 * SPDX-License-Identifier: 0BSD OR Apache-2.0
 * Copyright (C) 2026 Jon Mayo <jon@rm-f.net>
 */

/*
 * Consuming the startup record, per section 20.4.
 *
 * crt1 calls this with whatever the loader left in the first argument
 * register, before main() and before anything else in the library has run.
 * What it does is copy: the record and its array live in memory the loader
 * owns, and a library that kept pointers into it would be reading someone
 * else's storage for the life of the process. Copying costs one small
 * array and removes the question entirely.
 *
 * A record whose version is not the one this library was built against is
 * refused whole rather than read field by field. A program launched by a
 * loader it does not understand should hold nothing, which leaves it
 * unable to reach anything, which is the correct outcome for a process
 * that cannot trust what it was told.
 */

#include <ns.h>

#include <stddef.h>

static sys_ns_entry_t ns_table[SYS_NS_MAX];
static unsigned int ns_n;

void
__libc_startup(const sys_startup_t *startup)
{
    unsigned int n, i;

    ns_n = 0;
    if (startup == NULL || startup->version != SYS_STARTUP_VERSION) {
        return;
    }

    n = startup->ns_count;
    if (n > SYS_NS_MAX) {
        n = SYS_NS_MAX;
    }
    if (n != 0 && startup->ns == 0) {
        return;
    }

    for (i = 0; i < n; i++) {
        ns_table[i] = ((const sys_ns_entry_t *)startup->ns)[i];
        ns_table[i].name[SYS_NS_NAME_MAX - 1] = '\0';
    }
    ns_n = n;
}

sys_handle_t
ns_lookup(const char *name)
{
    unsigned int i;
    size_t j;

    if (name == NULL) {
        return SYS_HANDLE_INVALID;
    }

    for (i = 0; i < ns_n; i++) {
        for (j = 0; j < SYS_NS_NAME_MAX; j++) {
            if (ns_table[i].name[j] != name[j]) {
                break;
            }
            if (name[j] == '\0') {
                return ns_table[i].handle;
            }
        }
    }

    return SYS_HANDLE_INVALID;
}

unsigned int
ns_count(void)
{
    return ns_n;
}

const sys_ns_entry_t *
ns_entry(unsigned int i)
{
    if (i >= ns_n) {
        return NULL;
    }
    return &ns_table[i];
}
