/*
 * SPDX-License-Identifier: 0BSD OR Apache-2.0
 * Copyright (C) 2026 Jon Mayo <jon@rm-f.net>
 */

/*
 * The namespace a program was launched with, per section 20.4.
 *
 * The C library consumes the startup table before main() runs and keeps
 * the entries here. This is the whole of naming at this stage: a lookup by
 * identifier against the table the process was given, with no resolution,
 * no mount table and no path syntax. Section 20 is what it grows into, and
 * the property it already has is the one that matters, which is that an
 * identifier matching no entry reaches nothing at all.
 *
 * Identifiers below SYS_NS_PRIVATE are assigned in sys/startup.h and mean
 * the same thing everywhere. Above it they mean whatever the parent and
 * the child agreed, which is a library-level convention the kernel has no
 * part in.
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
 * Where a namespace entry is mounted, per section 20.4, or the empty
 * string for an entry that is only a role. Slot numbers are the ones
 * ns_slot walks.
 *
 * This is the whole of naming at this stage. What it grows into is a
 * mount table with a resolver over it, which is the next stage; what it
 * already has is the property that matters, which is that the name came
 * from the parent rather than from a convention the child assumed.
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
 * Resolve a path against the namespace this process was given, per
 * section 20.3.
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

/* The name an entry with this role is mounted at, or the empty string. */
const char *ns_name_of(sys_ns_id_t id);

#endif /* _NS_H_ */
