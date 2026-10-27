/* ns.h : namespace lookup, path resolution, and spawn composition.
 *
 * SPDX-License-Identifier: 0BSD OR Apache-2.0
 * Copyright (C) 2026 Jon Mayo <jon@rm-f.net>
 *
 * Declares __libc_startup(), which the C runtime calls with the startup
 * table, and the lookups over that table: ns_lookup(), ns_slots(),
 * ns_slot(), and ns_name(). Declares ns_resolve(), ns_resolve_at(), and
 * ns_name_of() for path resolution, and ns_compose() with the ns_how_t
 * choices (NS_NEW, NS_SHARE, NS_CLONE) for building the handle array a
 * spawn receives. Also defines NS_PATH_MAX. Included by many utilities
 * and by the C library.
 *
 */

#ifndef _NS_H_
#define _NS_H_

#include <sys/startup.h>

#include <stddef.h>

/*
 * Called by crt1 with whatever the loader left in the first argument
 * register, before main() and before anything else in the library has run.
 */
void __libc_startup(const sys_startup_t *startup);

/*
 * The handle an identifier names, or SYS_HANDLE_INVALID. A miss is an
 * ordinary result and not an error: a process is meant to be launchable
 * with a namespace that does not contain what some optional feature wants.
 */
sys_handle_t ns_lookup(sys_ns_id_t id);

/*
 * Slots in the table, for a program that wants to walk it rather than ask
 * for one thing. A slot may be unused, in which case ns_slot() writes
 * SYS_NS_NONE.
 */
unsigned int ns_slots(void);
void ns_slot(unsigned int i, sys_ns_id_t *id, sys_handle_t *handle);

/*
 * Where a namespace entry is mounted, or the empty
 * string for an entry that is only a role. Slot numbers are the ones
 * ns_slot walks.
 *
 * The name is the one the parent wrote, rather than a convention the
 * child assumed.
 */
const char *ns_name(unsigned int slot);

/*
 * The longest path this library will hold in canonical form. A bound
 * like every other bound here, and it is the library's rather than the
 * protocol's: what crosses a boundary is a handle and the part of a path
 * below it, which is shorter than this by construction.
 */
#define NS_PATH_MAX     128

/*
 * Resolve a path against the namespace this process was given.
 *
 * What comes back is one of the handles the process already holds and
 * the part of the path relative to it, which is exactly what a message
 * to a server carries. No message is sent and nothing is cached: the
 * table is the set of handles this process was given, so there is
 * nothing to invalidate and nothing to ask.
 *
 * A path that reduces outside every entry returns SYS_ERR_NO_ENT and
 * writes nothing. That is not an error condition so much as the shape of
 * the model: a name is not authority, so a name matching nothing reaches
 * nothing.
 */
sys_err_t ns_resolve(const char *path, sys_handle_t *out_dir, char *rest,
  size_t rest_len);

/*
 * The same, for the nth handle mounted at that name.
 *
 * Several entries under one name is a union directory,
 * and this is the whole of the mechanism: they are tried in the order
 * the parent wrote them until one answers. Only entries at the same name
 * are in the union. An entry with a shorter name is a different mount
 * rather than a fallback, because falling through to it would let a name
 * reach a place the parent deliberately mounted something else over.
 *
 * SYS_ERR_NO_ENT means there is no nth one, which is how a caller walking
 * them knows to stop.
 */
sys_err_t ns_resolve_at(const char *path, unsigned int nth,
  sys_handle_t *out_dir, char *rest, size_t rest_len);

/* The name an entry with this role is mounted at, or the empty string. */
const char *ns_name_of(sys_ns_id_t id);

/*
 * What to hand a child. These are choices about what
 * array to pass rather than mechanisms: there is nothing for the kernel
 * to do, because a namespace is the set of handles a parent decided to
 * give away and the names it gave them.
 */
typedef enum ns_how {
    NS_NEW = 0,     /* nothing, and a child that can name nothing */
    NS_SHARE,       /* every entry this process holds, under its names */
    NS_CLONE,       /* the entries whose names the caller lists */
} ns_how_t;

/*
 * Build the array for a spawn. The handles in it are duplicates, because
 * spawn moves what it is given and a parent that handed over its own
 * root would cease to have one; a caller whose spawn fails owns them and
 * closes them.
 *
 * The rights are the caller's decision and not this function's. A parent
 * may hand on less than it holds, and what to keep back
 * is a policy: a shell that strips the right to duplicate hands its
 * children a namespace they may use and may not pass on further, which
 * is a reasonable thing to want and a terrible thing for a library to
 * choose on a caller's behalf.
 *
 * More entries than the caller made room for is refused rather than
 * truncated. A child holding most of a namespace cannot tell that from a
 * child holding all of one, and the difference is what it can reach.
 */
sys_err_t ns_compose(ns_how_t how, const char *const *only,
  unsigned int nonly, uint32_t rights, sys_ns_spec_t *out,
  unsigned int max, unsigned int *nout);

#endif /* _NS_H_ */
