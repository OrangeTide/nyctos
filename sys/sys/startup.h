/*
 * SPDX-License-Identifier: 0BSD OR Apache-2.0
 * Copyright (C) 2026 Jon Mayo <jon@rm-f.net>
 */

/*
 * What a program is handed at launch, per section 20.4.
 *
 * A process receives its namespace as a table of handles and receives
 * nothing else. The loader places this record in the child's memory, the
 * entry point is called with a pointer to it, and the C library consumes
 * the table before main() runs, exactly the way envp is delivered.
 *
 * It is a record rather than a convention because a handle value means
 * nothing outside the table holding it. A child cannot be told what it
 * holds by any means other than being told, and it cannot name anything it
 * was not told about, which is what section 20.3 means by there being no
 * ambient root.
 *
 * This is the shared kernel and userland interface, like sys/op.h, so the
 * names follow the C header of section 26 rather than the project's usual
 * preference against typedefs of structures.
 */

#ifndef _SYS_STARTUP_H_
#define _SYS_STARTUP_H_

#include <sys/op.h>

#include <stddef.h>
#include <stdint.h>

#define SYS_STARTUP_VERSION     1u

/*
 * The table is a flat array of handle-sized words in pairs: an identifier
 * followed by the handle it names. A pair whose identifier is SYS_NS_NONE
 * is an unused slot and is skipped rather than ending the table, so a
 * parent may leave gaps and a child may drop an entry without compacting.
 *
 * Flat and small is the point. A string-keyed table costs a name buffer
 * per entry and a comparison per lookup, which is the wrong trade on a
 * part where the whole startup record has to fit in a few hundred bytes.
 * Sixteen bytes a slot is the price of the 20/44 identifier layout of
 * section 7.1 and nothing else.
 */
#define SYS_NS_MAX              8       /* slots, not bytes */
#define SYS_NS_WORDS            (SYS_NS_MAX * 2)

/*
 * Identifiers below SYS_NS_PRIVATE mean the same thing in every process
 * and are assigned here. Everything at or above it is arbitrary and means
 * whatever a parent and its child have agreed, which in practice means
 * whatever the library that spawned the child put there. The kernel
 * attaches no meaning to any of them; it copies the table and stops.
 */
typedef enum sys_ns_id {
    SYS_NS_NONE     = 0,        /* unused slot */

    /* Directories. Section 20.3: there is no ambient root, so a process
     * that was given neither of these can name nothing at all. */
    SYS_NS_ROOT     = 1,        /* what this process treats as its root */
    SYS_NS_CWD      = 2,        /* what relative names resolve against */

    /* Services this process consumes. */
    SYS_NS_CONSOLE  = 3,

    /* The channel this process receives on, for a process that is a
     * server. There is no operation for creating a channel, so a server is
     * given one at spawn or is not a server. */
    SYS_NS_SERVICE  = 4,

    SYS_NS_SYSTEM_MAX = 0x0fff,
    SYS_NS_PRIVATE    = 0x1000,
} sys_ns_id_t;

/*
 * The record itself. argv and envp are here because section 20.4 says the
 * namespace arrives alongside them, and a record that has to grow a field
 * later is a record every loader has to be taught twice. They are zero
 * until something passes arguments. They cost sixteen bytes once, not
 * once per entry, which is why trimming the table did not touch them.
 *
 * version is checked by the library rather than assumed. A program loaded
 * by a kernel that writes a record it does not understand should refuse to
 * read the table at all, which is cheaper than discovering the
 * disagreement one field at a time.
 */
typedef struct sys_startup {
    uint32_t        version;
    uint32_t        ns_words;   /* words in the table, not pairs */
    int32_t         argc;
    uint32_t        _reserved;
    uintptr_t       argv;       /* char ** */
    uintptr_t       envp;       /* char ** */
    uintptr_t       ns;         /* sys_handle_t *, ns_words of them */
} sys_startup_t;

/*
 * Find the handle an identifier names, or SYS_HANDLE_INVALID. Inline and
 * shared, so the kernel that writes a table and the library that reads one
 * cannot disagree about how a table is walked. A slot whose identifier is
 * SYS_NS_NONE is skipped, not treated as the end.
 */
static inline sys_handle_t
sys_ns_find(const sys_handle_t *ns, uint32_t words, sys_ns_id_t id)
{
    uint32_t i;

    if (ns == NULL || id == SYS_NS_NONE) {
        return SYS_HANDLE_INVALID;
    }
    for (i = 0; i + 1 < words; i += 2) {
        if (ns[i] == (sys_handle_t)id) {
            return ns[i + 1];
        }
    }

    return SYS_HANDLE_INVALID;
}

_Static_assert(sizeof(sys_startup_t) == 16 + 3 * sizeof(uintptr_t),
  "sys_startup_t is an ABI structure: four words then three pointers");

#endif /* _SYS_STARTUP_H_ */
