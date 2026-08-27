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

#include <sys/grant.h>
#include <sys/handle.h>
#include <sys/proc.h>
#include <sys/systm.h>
#include <sys/thread.h>
#include <sys/timeline.h>

void
handle_object_retain(uint32_t type, void *obj, uint32_t rights)
{
    if (type == SYS_OBJ_TIMELINE && (rights & SYS_RIGHT_SIGNAL) != 0) {
        timeline_add_signaler(obj);
    }

    /*
     * A completion timeline lives inside its thread or process, so a
     * handle naming it is a pointer into that object and has to hold
     * it. Section 10.4 wants the object reaped with the handle still
     * reporting the exit; what a reap can find is the difference, and
     * it can find nothing, so the object stays instead.
     */
    if (type == SYS_OBJ_TIMELINE) {
        void *owner;

        switch (timeline_owner(obj, &owner)) {
        case SYS_OBJ_PROCESS:
            proc_ref(owner);
            break;
        case SYS_OBJ_THREAD:
            thread_ref(owner);
            break;
        default:
            break;
        }
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
        proc_ref(obj);
    }

    if (type == SYS_OBJ_THREAD) {
        thread_ref(obj);
    }

    if (type == SYS_OBJ_CONNECTION) {
        connection_ref(obj);
    }
}

void
handle_object_release(const struct handle_table *ht, uint32_t type,
  void *obj, uint32_t rights)
{
    if (type == SYS_OBJ_TIMELINE && (rights & SYS_RIGHT_SIGNAL) != 0) {
        timeline_drop_signaler(obj);
    }

    if (type == SYS_OBJ_TIMELINE) {
        void *owner;
        uint32_t of = timeline_owner(obj, &owner);

        /*
         * Last, for the same reason the process case below is: this
         * can reap the object the timeline is inside, and nothing may
         * touch it afterwards.
         */
        if (of == SYS_OBJ_PROCESS) {
            proc_unref(owner);
        } else if (of == SYS_OBJ_THREAD) {
            thread_unref(owner);
        }
    }

    /*
     * A grant that was mapped stops being mapped. Section 7 has the
     * handle be the authority, and a region that stayed reachable after
     * the handle went would be a driver keeping its device by dropping
     * the thing that said it could have it.
     *
     * The mapping is taken back rather than removed: the region reverts
     * to what every other address space has for it, which is the
     * kernel's own mapping without the bit that lets user mode in. It is
     * done for a handle that was never mapped too, because nothing
     * records which of them was, and putting back what was already there
     * costs an entry written twice.
     */
    if (type == SYS_OBJ_GRANT && (rights & SYS_RIGHT_MAP) != 0) {
        grant_unmap(obj, ht->ht_proc);
    }

    /*
     * And a process or a thread loses the thing that could still ask
     * about it. Last, because dropping the last one reaps the object,
     * and nothing above may touch it afterwards.
     */
    if (type == SYS_OBJ_PROCESS) {
        proc_unref(obj);
    }

    if (type == SYS_OBJ_THREAD) {
        thread_unref(obj);
    }

    if (type == SYS_OBJ_CONNECTION) {
        connection_unref(obj);
    }
}
