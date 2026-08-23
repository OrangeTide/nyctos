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

/*
 * Where each of those handles is mounted, per section 20.4, copied for
 * the same reason the table is. Fixed slots rather than pointers, so
 * the copy is one memcpy and a name cannot outlive what it names.
 */
static char ns_names[SYS_NS_NAME_BYTES];

/*
 * The command line, copied for the same reason the namespace table is.
 * The record lives in loader memory, which at this tier is the process
 * object and at the next one is wherever section 15.2's loader put it;
 * either way a program that held pointers into it would be depending on
 * something it does not own. Copying also lets main() do what C programs
 * do to their arguments, which is edit them in place.
 */
static char arg_bytes[SYS_ARG_BYTES];
static char *arg_vec[SYS_MAX_ARGS + 1];
static int arg_count;


/*
 * Copy argc and argv out of the record. A string that does not fit ends
 * the list rather than being truncated: an argument cut in half is an
 * argument that means something else.
 */
static void
copy_args(const sys_startup_t *startup)
{
    const char *const *src = (const char *const *)startup->argv;
    uint32_t used = 0;
    int i;

    if (src == NULL || startup->argc <= 0) {
        return;
    }

    for (i = 0; i < startup->argc && i < SYS_MAX_ARGS; i++) {
        const char *s = src[i];
        uint32_t n = 0;

        if (s == NULL) {
            break;
        }
        while (s[n] != '\0') {
            n++;
        }
        if (used + n + 1 > sizeof(arg_bytes)) {
            break;
        }

        for (n = 0; s[n] != '\0'; n++) {
            arg_bytes[used + n] = s[n];
        }
        arg_bytes[used + n] = '\0';
        arg_vec[i] = &arg_bytes[used];
        used += n + 1;
    }

    arg_vec[i] = NULL;
    arg_count = i;
}

void
__libc_startup(const sys_startup_t *startup)
{
    const sys_handle_t *src;
    uint32_t n, i;

    ns_words = 0;
    arg_count = 0;
    arg_vec[0] = NULL;
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

    /*
     * The names beside the table. A loader that wrote a table and no
     * names is one where nothing is mounted anywhere, which is a
     * process that can hold handles and name none of them.
     */
    if (startup->ns_names != 0) {
        const char *from = (const char *)startup->ns_names;

        for (i = 0; i < SYS_NS_NAME_BYTES; i++) {
            ns_names[i] = from[i];
        }
    }

    copy_args(startup);
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

/*
 * What crt1 puts in the first two argument registers before it calls
 * main(). Two calls rather than one structure, because the alternative is
 * assembly that knows the shape of a C type.
 */
int
__libc_argc(void)
{
    return arg_count;
}

char **
__libc_argv(void)
{
    return arg_vec;
}

const char *
ns_name(unsigned int slot)
{
    return sys_ns_name(ns_names, ns_words, slot);
}
