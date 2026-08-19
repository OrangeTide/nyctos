/*
 * SPDX-License-Identifier: 0BSD OR Apache-2.0
 * Copyright (C) 2026 Jon Mayo <jon@rm-f.net>
 */

/*
 * What a handle coming or going means to the object it names.
 *
 * Most objects do not care. A timeline does, because section 9.3 makes the
 * loss of the last signal-righted handle an event: the party that could
 * have satisfied a wait has gone, and a waiter must be told rather than
 * left to a timeout.
 *
 * This lives apart from the handle table so that the table stays generic
 * and does not grow a case per object type.
 */

#include <sys/handle.h>
#include <sys/systm.h>
#include <sys/timeline.h>

void
handle_object_retain(uint32_t type, void *obj, uint32_t rights)
{
    if (type == SYS_OBJ_TIMELINE && (rights & SYS_RIGHT_SIGNAL) != 0) {
        timeline_add_signaler(obj);
    }
}

void
handle_object_release(uint32_t type, void *obj, uint32_t rights)
{
    if (type == SYS_OBJ_TIMELINE && (rights & SYS_RIGHT_SIGNAL) != 0) {
        timeline_drop_signaler(obj);
    }
}
