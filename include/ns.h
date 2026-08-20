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

#endif /* _NS_H_ */
