/* startup.h : startup record handed to a new program.
 *
 * SPDX-License-Identifier: 0BSD OR Apache-2.0
 * Copyright (C) 2026 Jon Mayo <jon@rm-f.net>
 *
 * Declares the startup record the loader passes to a program. It holds a
 * namespace table of identifier and handle pairs, with optional names, and the
 * sys_ns_* types and lookup functions. Included by sys/proc.h, sys/peer.h,
 * sys/systm.h, include/ns.h and include/os/sys.h.
 *
 */

#ifndef _SYS_STARTUP_H_
#define _SYS_STARTUP_H_

#include <sys/op.h>

#include <stddef.h>
#include <stdint.h>

#define SYS_STARTUP_VERSION     2u

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
 * A name per entry, and why the argument above no longer applies.
 *
 * The comment above defends a table keyed by number: a string-keyed one
 * costs a name buffer per entry and a comparison per lookup. That was
 * right while an entry was a role a parent and child agreed on, where the
 * number is the whole of the meaning and a name would be decoration.
 *
 * Section 20.4 asks for something else beside it: the set of directory
 * handles this process was given, with names attached, which is what a
 * mount table is. A parent that cannot say where to mount a handle cannot
 * compose a namespace, and composing one is the milestone. So an entry
 * may carry a name, and an entry that is only a role carries none.
 *
 * The cost is bounded and paid once: sixteen bytes a slot, beside the two
 * hundred and fifty-six the record already carries for a command line. It
 * buys the ability to write a path.
 */
#define SYS_NS_NAME_MAX         16      /* bytes, including the terminator */
#define SYS_NS_NAME_BYTES       (SYS_NS_MAX * SYS_NS_NAME_MAX)

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

    /*
     * The device a driver drives, as a grant over its registers per
     * section 19.2. A driver holds one of these and a program holds
     * none, which is the whole of the difference between them.
     */
    SYS_NS_DEVICE   = 5,

    /*
     * The pager this process faults to, per section 15.3. A process
     * given none cannot be paged and dies of a fault instead, which
     * is what every process in this system did before there was a
     * pager to give.
     */
    SYS_NS_PAGER    = 6,

    /*
     * The block store a server keeps its data in, as a connection to
     * the driver that owns it. Not SYS_NS_DEVICE, which is a grant over
     * registers and is what a driver holds; this is what a layer above
     * a driver holds, which is an ordinary connection to an ordinary
     * server. Section 19.1: a driver is a process, so talking to one is
     * talking to a process.
     */
    SYS_NS_STORE    = 7,
    SYS_NS_SYSTEM_MAX = 0x0fff,
    SYS_NS_PRIVATE    = 0x1000,
} sys_ns_id_t;

/*
 * One entry of what a parent hands a child: an identifier and the handle
 * it names. The handle is moved rather than copied, per section 7.4, so
 * an array of these is a list of what the parent is giving up.
 *
 * The table the child reads is flat words, and this is not that. A
 * parent writes pairs and the kernel flattens them, because the array a
 * caller builds is easier to get right when the two halves cannot drift
 * apart in it.
 */
typedef struct sys_ns_spec {
    sys_ns_id_t     id;
    sys_handle_t    handle;

    /*
     * Where this handle is mounted, or an empty string for an entry that
     * is only a role.
     *
     * Inline rather than a pointer, and the reason is worth keeping. A
     * pointer is a second thing the kernel has to follow out of a
     * caller's memory, which is section 23.2's problem again, and it is a
     * field every existing caller of spawn would have left uninitialised:
     * adding one turned fifteen working call sites into fifteen wild
     * reads, which is what happened. Sixteen bytes in the caller's array
     * costs a little stack and cannot point anywhere.
     */
    char            name[SYS_NS_NAME_MAX];
} sys_ns_spec_t;

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

    /*
     * The names, one fixed-size slot per pair in the table above,
     * indexed the same way. A slot is empty for an entry that has no
     * name. Fixed slots rather than pointers because the loader
     * copies the block and a pointer per entry would have to be
     * rewritten per entry, which is what argv costs and what a
     * namespace does not have to.
     */
    uintptr_t       ns_names;   /* char *, SYS_NS_NAME_BYTES of them */

    /*
     * The system page of section 6.2, mapped read-only into this
     * process. An address rather than a fixed location, for the reason
     * that section gives: what a program dereferences comes from its
     * own description of itself, so nothing is assumed about where a
     * page lands in an address space that is not shared.
     *
     * Zero for a process that has none, which is any process the
     * kernel built rather than loaded.
     */
    uintptr_t       syspage;
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

/*
 * The name in slot i, which is the empty string for an entry that has
 * none. Inline and shared for the same reason sys_ns_find is: the kernel
 * that writes a block and the library that reads one cannot disagree
 * about how it is indexed.
 */
static inline const char *
sys_ns_name(const char *names, uint32_t words, uint32_t slot)
{
    if (names == NULL || slot >= words / 2 || slot >= SYS_NS_MAX) {
        return "";
    }

    return names + (size_t)slot * SYS_NS_NAME_MAX;
}

_Static_assert(sizeof(sys_startup_t) == 16 + 5 * sizeof(uintptr_t),
  "sys_startup_t is an ABI structure: four words then four pointers");

#endif /* _SYS_STARTUP_H_ */
