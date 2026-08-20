/*
 * SPDX-License-Identifier: 0BSD OR Apache-2.0
 * Copyright (C) 2026 Jon Mayo <jon@rm-f.net>
 */

/*
 * Consuming the startup record, per section 20.4.
 *
 * crt1 calls this with whatever the loader left in the first argument
 * register, before main() and before anything else in the library has run.
 * What it does is copy: the record and its table live in memory the loader
 * owns, and a library that kept a pointer into it would be reading someone
 * else's storage for the life of the process. The table is a flat array of
 * identifier and handle pairs, so the copy is a few dozen bytes and needs
 * no allocation.
 *
 * A record whose version is not the one this library was built against is
 * refused whole rather than read field by field. A program launched by a
 * loader it does not understand should hold nothing, which leaves it
 * unable to reach anything, which is the correct outcome for a process
 * that cannot trust what it was told.
 */

#include <ns.h>

#include <stddef.h>

static sys_handle_t ns_table[SYS_NS_WORDS];
static uint32_t ns_words;

void
__libc_startup(const sys_startup_t *startup)
{
    const sys_handle_t *src;
    uint32_t n, i;

    ns_words = 0;
    if (startup == NULL || startup->version != SYS_STARTUP_VERSION) {
        return;
    }

    n = startup->ns_words;
    if (n > SYS_NS_WORDS) {
        n = SYS_NS_WORDS;
    }
    n &= ~1u;                   /* a trailing half-pair names nothing */
    if (n == 0 || startup->ns == 0) {
        return;
    }

    src = (const sys_handle_t *)startup->ns;
    for (i = 0; i < n; i++) {
        ns_table[i] = src[i];
    }
    ns_words = n;
}

sys_handle_t
ns_lookup(sys_ns_id_t id)
{
    return sys_ns_find(ns_table, ns_words, id);
}

unsigned int
ns_slots(void)
{
    return ns_words / 2;
}

void
ns_slot(unsigned int i, sys_ns_id_t *id, sys_handle_t *handle)
{
    int present = i * 2 + 1 < ns_words;

    if (id != NULL) {
        *id = present ? (sys_ns_id_t)ns_table[i * 2] : SYS_NS_NONE;
    }
    if (handle != NULL) {
        *handle = present ? ns_table[i * 2 + 1] : SYS_HANDLE_INVALID;
    }
}
