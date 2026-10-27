/* objref.h : exclusion for object reference counts.
 *
 * SPDX-License-Identifier: 0BSD OR Apache-2.0
 * Copyright (C) 2026 Jon Mayo <jon@rm-f.net>
 *
 * Declares objref_enter and objref_leave, which bracket the reference count and
 * the free decision for process, thread and connection objects. Included by
 * the process, thread, connection and channel sources.
 *
 */

#ifndef _SYS_OBJREF_H_
#define _SYS_OBJREF_H_

#include <sys/intr.h>

intr_state_t objref_enter(void);
void objref_leave(intr_state_t saved);

#endif /* _SYS_OBJREF_H_ */
