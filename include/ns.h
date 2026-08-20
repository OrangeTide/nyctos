/*
 * SPDX-License-Identifier: 0BSD OR Apache-2.0
 * Copyright (C) 2026 Jon Mayo <jon@rm-f.net>
 */

/*
 * The namespace a program was launched with, per section 20.4.
 *
 * The C library consumes the startup record before main() runs and keeps
 * the entries here. This is the whole of naming at this stage: a lookup
 * against the array the process was given, with no resolution, no mount
 * table and no path syntax. Section 20 is what it grows into, and the
 * property it already has is the one that matters, which is that a name
 * that matches no entry reaches nothing at all.
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
 * The handle bound to a name, or SYS_ID_INVALID. A miss is SYS_HANDLE_INVALID, an ordinary
 * result and not an error: a process is meant to be launchable with a
 * namespace that does not contain what some optional feature wants.
 */
sys_handle_t ns_lookup(const char *name);

/* How many entries the process was given, for a program that wants to walk
 * them rather than ask for one. */
unsigned int ns_count(void);

/* Entry i, or NULL if i is out of range. */
const sys_ns_entry_t *ns_entry(unsigned int i);

#endif /* _NS_H_ */
