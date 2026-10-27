/* startup.c : consumes the startup record and exposes its contents.
 *
 * SPDX-License-Identifier: 0BSD OR Apache-2.0
 * Copyright (C) 2026 Jon Mayo <jon@rm-f.net>
 *
 * __libc_startup, called from crt1, copies the namespace table, the namespace
 * names, the command line, and the system page pointer from the startup
 * record. A record with a different version is ignored, which leaves the
 * namespace and arguments empty. The accessors for this state are ns_lookup,
 * ns_slots, ns_slot, ns_name, ns_name_of, __libc_argc, __libc_argv, and
 * sys_syspage.
 *
 */

#include <ns.h>
#include <os/sys.h>

#include <stddef.h>

static sys_handle_t ns_table[SYS_NS_WORDS];

/*
 * The system page, from the startup record. Kept because
 * the record itself is not: this library copies what it needs out of it
 * and the record's address is not something a program is given twice.
 *
 * NULL for a process the kernel built rather than loaded, and for one
 * whose record this library refused, so a caller checks rather than
 * assuming every process has one.
 */
static const sys_syspage_t *syspage;
static uint32_t ns_words;

/*
 * Where each of those handles is mounted, copied for
 * the same reason the table is. Fixed slots rather than pointers, so
 * the copy is one memcpy and a name cannot outlive what it names.
 */
static char ns_names[SYS_NS_NAME_BYTES];

/*
 * The command line, copied for the same reason the namespace table is.
 * The record lives in loader memory, which at this tier is the process
 * object and at the next one is wherever the loader put it;
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
    syspage = NULL;
    if (startup == NULL || startup->version != SYS_STARTUP_VERSION) {
        return;
    }

    /*
     * Before the namespace, because it is one word and needs no
     * copying: the page is mapped for as long as this process is, so
     * keeping the address is keeping all of it.
     */
    syspage = (const sys_syspage_t *)startup->syspage;

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
     * The names beside the table, one slot per pair and no more than the
     * table has pairs. Bounded by what the record says exists rather than
     * by the size of this library's array: a loader that writes a shorter
     * block is a loader this reads past the end of, and the record says
     * how many entries there are in exactly one place.
     */
    if (startup->ns_names != 0) {
        const char *from = (const char *)startup->ns_names;
        uint32_t bytes = (n / 2) * SYS_NS_NAME_MAX;

        for (i = 0; i < bytes; i++) {
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

const char *
ns_name_of(sys_ns_id_t id)
{
    unsigned int i;

    for (i = 0; i < ns_words / 2; i++) {
        if ((sys_ns_id_t)ns_table[i * 2] == id) {
            return ns_name(i);
        }
    }

    return "";
}

/*
 * The system page, or NULL for a process that has none.
 *
 * A program reaches it through its own description of
 * itself rather than at a fixed address, and this is where that
 * description was read. A caller checks for NULL because a process the
 * kernel built rather than loaded has no page and no record.
 */
const sys_syspage_t *
sys_syspage(void)
{
    return syspage;
}
