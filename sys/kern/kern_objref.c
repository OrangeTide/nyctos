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
#include <sys/proc.h>
#include <sys/systm.h>
#include <sys/timeline.h>

void
handle_object_retain(uint32_t type, void *obj, uint32_t rights)
{
    if (type == SYS_OBJ_TIMELINE && (rights & SYS_RIGHT_SIGNAL) != 0) {
        timeline_add_signaler(obj);
    }

    /*
     * A handle to a process claims its exit. Section 10.4 keeps the
     * status readable through the handle after the object has
     * finished, so holding one is what makes an exit something
     * somebody can still ask about, and an exit nobody can ask about
     * halts the machine until section 24.3 has a parent to decide.
     *
     * It does not come back off when the handle closes. The question
     * is whether anyone ever undertook to look, and closing a handle
     * does not un-ask it.
     */
    if (type == SYS_OBJ_PROCESS) {
        proc_watch(obj);
    }
}

void
handle_object_release(uint32_t type, void *obj, uint32_t rights)
{
    if (type == SYS_OBJ_TIMELINE && (rights & SYS_RIGHT_SIGNAL) != 0) {
        timeline_drop_signaler(obj);
    }
}
