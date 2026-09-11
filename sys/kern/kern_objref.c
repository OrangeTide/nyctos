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

#include <sys/lock.h>
#include <sys/objref.h>
#include <sys/prepared.h>
#include <sys/grant.h>
#include <sys/channel.h>
#include <sys/handle.h>
#include <sys/proc.h>
#include <sys/systm.h>
#include <sys/thread.h>
#include <sys/timeline.h>

void
handle_object_retain(const struct handle_table *ht, uint32_t type,
  void *obj, uint32_t rights)
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
        connection_retain(obj, ht->ht_proc);
    }

    /*
     * A channel belongs to whoever may receive on it, per section 12.5
     * read against section 24.5: root creates a channel and hands the
     * receive end to a server, and the process whose death matters to
     * a blocked client is the one that was going to answer it, not the
     * one that made the object.
     *
     * Which is also the only way round the circle. A program learns
     * its handles at spawn, so a channel it receives on has to exist
     * before it does, so it cannot be the creator; and until this, the
     * creator was the owner and a server dying released nobody.
     */
    if (type == SYS_OBJ_CHANNEL && (rights & SYS_RIGHT_RECV) != 0) {
        channel_set_owner(obj, ht->ht_proc);
    }

    /*
     * And the channel is held by the handle, per M20 stage two.
     * Whatever the rights: a send handle is as good a reason not to
     * take the slot back as a receive handle, and better, since it is
     * the one a client holds.
     */
    if (type == SYS_OBJ_CHANNEL) {
        channel_hold(obj);
    }
}

void
handle_object_release(const struct handle_table *ht, uint32_t type,
  void *obj, uint32_t rights)
{
    /*
     * A prepared program is its handle, per section 17.7: nothing else
     * names one, so the slot goes back when the handle does. It did
     * not, and four slots meant the fifth program a process ever
     * loaded was refused for want of room that nothing was using.
     */
    if (type == SYS_OBJ_PREPARED) {
        prepared_free(obj);
    }

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
        connection_release(obj, ht->ht_proc);
    }

    /*
     * And losing the receive right is no longer being the one who
     * answers. Both ways round, because ownership is the right and not
     * a record of who once had it: a channel whose server has handed
     * the right on belongs to whoever has it now, and one whose server
     * has dropped it belongs to nobody.
     */
    if (type == SYS_OBJ_CHANNEL && (rights & SYS_RIGHT_RECV) != 0 &&
      channel_owner(obj) == ht->ht_proc) {
        channel_set_owner(obj, NULL);
    }

    /* And the handle that held it is gone, per M20 stage two. */
    if (type == SYS_OBJ_CHANNEL) {
        channel_drop(obj);
    }
}

#if SYS_TIER == 2

static struct spinlock objref_lock = { 0, 0, "objrefs" };

intr_state_t
objref_enter(void)
{
    intr_state_t s = md_intr_disable();

    spin_lock(&objref_lock);

    return s;
}

void
objref_leave(intr_state_t saved)
{
    spin_unlock(&objref_lock);
    md_intr_restore(saved);
}

#else

/*
 * Section 13.3 buys Tier 0 a kernel with no internal locking at all,
 * and one core is what makes disabling interrupts exclusion again.
 */
intr_state_t
objref_enter(void)
{
    return md_intr_disable();
}

void
objref_leave(intr_state_t saved)
{
    md_intr_restore(saved);
}

#endif /* SYS_TIER == 2 */
