/*
 * SPDX-License-Identifier: 0BSD OR Apache-2.0
 * Copyright (C) 2026 Jon Mayo <jon@rm-f.net>
 */

/*
 * Send, receive, reply. See sys/sys/channel.h for the shape and section 12
 * for the reasoning.
 */

#include <sys/measure.h>
#include <sys/cpu.h>
#include <sys/channel.h>
#include <sys/intr.h>
#include <sys/lock.h>
#include <sys/connection.h>
#include <sys/handle.h>
#include <sys/object.h>
#include <sys/objref.h>
#include <sys/proc.h>
#include <sys/sched.h>
#include <sys/systm.h>
#include <sys/trace.h>
#include <sys/thread.h>
#include <sys/vm.h>
#include <sys/timeline.h>

struct channel *
channel_create(const char *name, struct proc *owner)
{
    struct channel *ch;

    ch = pool_alloc(channel_pool(), NULL);
    if (ch == NULL) {
        return NULL;
    }

    /*
     * Counted under the same lock the holds are counted under,
     * because two cores can be making channels at once and a lost
     * increment here is a number in the compared report that moves
     * on its own.
     */
    {
        intr_state_t s = objref_enter();

        channel_created++;
        objref_leave(s);
    }

    ch->c_id = pool_id_of(channel_pool(), ch);
    ch->c_name = name;
    ch->c_owner = owner;
    /*
     * Every field, said rather than inherited, and none of it
     * required.
     *
     * M18 wrote four of these with a note that nothing had ever
     * needed writing, because a channel is never released and a pool
     * slot starts as zero, and that this would stop being true the
     * day a slot came back. M20 stage three is that day and the note
     * turns out to be wrong about why: `pool_alloc` clears the object
     * it hands out, every time and not only the first, so a recycled
     * slot arrives as zero exactly as a fresh one does.
     *
     * The state that would have been felt if it did not is
     * c_pulse_lost. A channel that had a pulse dropped and then
     * evicted the last one it was holding can go with that flag set,
     * and the next occupant would tell its server it had missed
     * something that happened before it existed. That is what the
     * check on a reused slot is for, and it passes because of the
     * clear rather than because of these lines.
     *
     * They stay because a reader of this function should be able to
     * see what a channel starts as without knowing what the pool does
     * with the memory first.
     */
    ch->c_senders = NULL;
    ch->c_receivers = NULL;
    ch->c_serving = NULL;
    ch->c_holds = 0;
    ch->c_conns = NULL;
    ch->c_reaping = false;
    ch->c_depth = 0;
    ch->c_bounded = false;
    ch->c_pulse_head = 0;
    ch->c_pulse_count = 0;
    ch->c_pulse_users = 0;
    ch->c_pulse_lost = false;

    return ch;
}

/*
 * How many senders are queued here, and which of them is lowest.
 *
 * One walk, because a bounded queue is surveyed only while it is at its
 * bound and an unbounded one is never surveyed at all, so the list is
 * short wherever this is asked.
 *
 * Lowest by priority rather than last in the list, and those are not
 * always the same thread. The list is ordered when a sender is inserted
 * and not afterwards, and section 12.3's chain raises a queued sender
 * in place: a thread that took a request and then sent on is raised
 * where it stands when somebody high priority arrives behind it, so its
 * priority goes up and its position does not. Taking the tail would
 * displace exactly the client the chain had just raised.
 *
 * The last of the lowest, so that arrival order still decides inside a
 * level: of two that are equally low, the one that has been waiting
 * less is the one that goes.
 */
static uint32_t
sender_survey(struct channel *ch, struct thread **lowest)
{
    struct thread *t;
    uint32_t n = 0;

    *lowest = NULL;
    for (t = ch->c_senders; t != NULL; t = t->t_snext) {
        if (*lowest == NULL || t->t_prio <= (*lowest)->t_prio) {
            *lowest = t;
        }
        n++;
    }

    return n;
}

/*
 * Senders a bounded queue would not hold. See channel.h for why the two
 * are counted apart when the client is told the same thing by both.
 */
uint64_t channel_senders_refused;
uint64_t channel_senders_displaced;

/* Departed peers a server collected. */
uint64_t channel_peers_collected;

/*
 * How many senders this channel will hold, per M18 stage six and
 * section 12.1. Nothing is evicted here: a depth lowered under a queue
 * that is already longer takes effect at the next send, which is the
 * one moment the queue is being looked at anyway.
 */
void
channel_listen(struct channel *ch, uint32_t depth)
{
    intr_state_t s = md_intr_disable();

    ch->c_depth = depth;
    ch->c_bounded = true;

    md_intr_restore(s);
}

/*
 * Insert into the sender queue by effective priority, highest first, and
 * FIFO within a priority. This ordering is the whole reason the queue
 * exists as a sorted structure rather than a list.
 */
static void
sender_insert(struct channel *ch, struct thread *t)
{
    struct thread **link = &ch->c_senders;

    while (*link != NULL && (*link)->t_prio >= t->t_prio) {
        link = &(*link)->t_snext;
    }

    t->t_snext = *link;
    *link = t;
}

static struct thread *
list_pop(struct thread **head)
{
    struct thread *t = *head;

    if (t != NULL) {
        *head = t->t_snext;
        t->t_snext = NULL;
    }

    return t;
}

static bool
list_remove(struct thread **head, struct thread *t)
{
    while (*head != NULL && *head != t) {
        head = &(*head)->t_snext;
    }
    if (*head != t) {
        return false;
    }

    *head = t->t_snext;
    t->t_snext = NULL;

    return true;
}

static void channel_thread_release(struct thread *t, bool tell);

/*
 * Count a send that gave up at its deadline, in whichever of section
 * 12.1's two states it was in.
 *
 * Counted here rather than in channel_unwait_thread, which is where the
 * two states are told apart, because that hook is reached by
 * cancellation as well: a thread taken off a channel because somebody
 * cancelled it is not a send that gave up, and counting it as one made
 * this figure exactly double what the checks ask for.
 *
 * A thread blocked on anything else is not counted at all, which is
 * what the two wait states are for.
 */
void
channel_gave_up(const struct thread *t)
{
    if (t->t_wait == THREAD_WAIT_REPLY) {
        channel_gave_up_working++;
    } else if (t->t_wait == THREAD_WAIT_SEND) {
        channel_gave_up_queued++;
    }
}


void
channel_unwait_thread(void *obj, struct thread *t)
{
    struct channel *ch = obj;

    /*
     * A client whose message a server is holding is on neither list:
     * the rendezvous already happened and what it is waiting for is the
     * reply. Section 12.1 has it abandon work in progress.
     *
     * Only the pointers here, and the rest when the thread runs again.
     * This is called from the timer interrupt, where thread_current()
     * is whatever the core was doing and may be nothing at all, and the
     * undonation and the buffer take-back both need a thread to charge
     * the work to. Doing them here wrote to offset 0x150 of a null
     * pointer, which is where t_charging lives.
     *
     * What has to happen now is what makes a late reply stale: with no
     * reply slot, channel_reply refuses. What can wait is the memory
     * and the priority, because nothing else looks at either until
     * this thread is running.
     */
    if (t->t_wait == THREAD_WAIT_REPLY) {
        t->t_msg = NULL;
        t->t_reply = NULL;
        t->t_blocked_on = NULL;
        return;
    }

    if (!list_remove(&ch->c_senders, t)) {
        (void)list_remove(&ch->c_receivers, t);
    }
    t->t_blocked_on = NULL;
}

/*
 * Raise a thread to at least prio.
 *
 * This is one step, and section 12.3 asks for a transitive chain. It gets
 * one anyway, by a different route: a server sends onward at its own
 * effective priority, which is already the donated value, so each hop
 * carries the donation forward without anything walking a chain. A client
 * at 25 reaching a server at 10 that calls a server at 5 leaves all three
 * running at 25.
 *
 * What that does not give is the bound. SYS_PRIO_CHAIN_MAX and the
 * SYS_ERR_DEADLOCK the section attaches to it presuppose a walk, and a
 * walk is only needed to raise a thread that is already blocked onward.
 * Nothing here can be: donation happens at the rendezvous, when the target
 * has just stopped being blocked. So the bound is unenforced rather than
 * enforced trivially, and a cycle of servers would deadlock undetected.
 * That is a real gap between section 12.3 and this, and it wants either a
 * link from a blocked sender to whoever is serving it, or the section
 * conceding that per-send donation is the whole mechanism.
 *
 * The earlier version of this walked from the target back to the thread
 * that had just donated to it, which by construction already outranked the
 * value being donated, so the loop always stopped on its first test. It
 * looked like a bounded chain walk and was a no-op.
 */
static void
donate(struct thread *to, uint8_t prio)
{
    if (to->t_prio < prio) {
        ipc_donate_stamp(to, to->t_core != cpu_self()->cp_index);
#if SYS_TIER == 2
        /*
         * Raised on a thread this core does not own, which is what
         * section 11.4 has cross a boundary. Counted because it is a
         * cost partitioning has and a single core does not.
         */
        if (to->t_core != cpu_self()->cp_index) {
            cpu_self()->cp_donate_across++;
        }
#endif
        sched_reprioritise(to, prio);
    }
}

/* Recompute a thread's priority now that it owes nobody anything. */
static void
undonate(struct thread *t)
{
    t->t_donor = NULL;
    if (t->t_prio != t->t_base_prio) {
        sched_reprioritise(t, t->t_base_prio);
    }
}

/* Whether a sender may describe this memory to this receiver. */
static bool buf_may_deliver(struct proc *from, struct proc *to, uintptr_t va,
  size_t len);

static void serving_begin(struct channel *ch, struct thread *t);
static void serving_end(struct thread *t);
static sys_err_t chain_raise(struct channel *ch, uint8_t prio);
static void release_blocked(struct thread *t, sys_err_t err);

static void
msg_copy(struct sys_msg *dst, const struct sys_msg *src)
{
    /*
     * A message copy chunk boundary, per section 13.3. This copy is
     * one fixed-size structure and is not chunked, so there is exactly
     * one boundary and it is here. A copy that grows into chunks puts
     * a point at each of them.
     */
    preempt_point();

    memcpy(dst, src, sizeof(*dst));
}

/*
 * Take a lent buffer back out of the space it was lent to and put it
 * back in the sender's, per section 6.1: the reply is the end of the
 * call and the point at which the right returns.
 *
 * The pages are found by asking the receiver's tables what its addresses
 * name, rather than by remembering them at the hand-over. A lend can be
 * any number of pages and they need not be one run of physical memory,
 * so remembering them would be a list per thread sized for the largest
 * buffer anyone might send; the receiver's own tables already hold
 * exactly this, and are the thing being undone.
 *
 * The record is on the client, which is the thread whose memory it is,
 * rather than on the receiver. A reply may come from any thread holding
 * the client's id, not only the one that received the message, and a
 * take-back keyed to the replying thread would find nothing to undo and
 * leave the pages in a process that is no longer looking at them.
 *
 * A client whose server never replies does not get its memory back. That
 * is a server that has stopped serving, and what to do about one is
 * section 24.3's question rather than this function's.
 */
static void
msg_buf_take_back(struct thread *client)
{
#if SYS_TIER == 2
    struct addrspace *from;
    struct addrspace *to;
    uintptr_t va;

    if (client->t_lend_len == 0) {
        return;
    }

    from = proc_as(client->t_lend_to);
    to = proc_as(client->t_proc);

    for (va = client->t_lend_va;
      va < client->t_lend_va + client->t_lend_len; va += PAGE_SIZE) {
        uintptr_t pa;

        if (md_lookup(from, va, &pa) != SYS_OK) {
            panic("ipc: the page lent at 0x%lx is not in %s any more",
              (unsigned long)va, client->t_lend_to->p_name);
        }
        md_unmap(from, va, PAGE_SIZE);
        if (md_map(to, va, pa, PAGE_SIZE,
          proc_prot(client->t_proc, va)) != SYS_OK) {
            /*
             * The tables the page hung from were left in place when it
             * was unmapped, so putting it back needs no frame and this
             * cannot fail. If it does, the client is about to run with
             * a hole where its memory was and there is nothing honest
             * to return to it.
             */
            panic("ipc: %s cannot have its own page at 0x%lx back",
              client->t_name, (unsigned long)va);
        }

        /*
         * With the owner's permissions, which is the client and not
         * the server it was lent to. Asking the borrower gives the
         * fallback for an address outside its own memory, and the
         * fallback is everything: the page came back executable, on
         * exactly the pages a program lends. Measured, the two answers
         * are 0xb and 0xf.
         */

        /*
         * The same address in both spaces, so one call again: it left
         * the borrower and came back to the owner at the address it
         * always had.
         */
        md_tlb_invalidate(va);
    }

    vm_charge_pages((uint32_t)(client->t_lend_len / PAGE_SIZE));

    client->t_lend_va = 0;
    client->t_lend_len = 0;
    client->t_lend_to = NULL;
#else
    (void)client;
#endif
}

#if SYS_TIER == 2

/*
 * What the receiver may do with the buffer, per section 6.1: the flags
 * in the message are the access it is being given and not a description
 * of what the sender has.
 *
 * Write implies read. Neither port can express a page that may be
 * written and not read, so a buffer asked for write-only would either be
 * refused or quietly widened, and widening it silently is the worse of
 * the two.
 */
static uint32_t
buf_perms(uint32_t buf_flags)
{
    uint32_t flags = VM_USER;

    if (buf_flags & SYS_BUF_WRITE) {
        flags |= VM_READ | VM_WRITE;
    } else if (buf_flags & SYS_BUF_READ) {
        flags |= VM_READ;
    }

    return flags;
}

#endif /* SYS_TIER == 2 */

/*
 * Hand a request's buffer to the receiver, per section 6.1.
 *
 * The address in the message is the sender's on the way in and the
 * receiver's on the way out. At Tier 0 that rewrite is the identity: one
 * address space, nothing to map, and a sender that breaks the contract by
 * touching the memory anyway will not fault.
 *
 * At Tier 2 the two are different memories, and this is what makes the
 * receiver able to read a buffer at all: the pages behind the sender's
 * address are mapped into the receiver's space at the same address, so
 * the number in the message keeps meaning what it meant. Keeping the
 * address is what lets a server dereference it directly, which is what
 * the fs protocol does and what section 25.5 says it does.
 *
 * It is a move and not a loan. The pages go out of the sender as they go
 * into the receiver, so a sender that touches its own buffer between the
 * send and the reply faults on it, which is section 6.1's rule with the
 * hardware behind it. Tier 0 keeps the rule and cannot keep it: there is
 * one address space, nothing to unmap, and a program that breaks the
 * contract there is not corrected. That difference is the whole of what
 * the two tiers are.
 */
static sys_err_t
msg_buf_hand_over(struct thread *server, struct thread *client,
  struct sys_msg *m)
{
    if (m->buf_len == 0) {
        m->buf = 0;
        m->buf_flags = 0;
        return SYS_OK;
    }

    /*
     * The same rule the send asked, now that there is a receiver to
     * ask it about. A member is let past the first asking because a
     * channel cannot say who will receive on it; this is where a
     * member sending the domain's memory to somebody outside the
     * domain is refused, and it is the only place that can.
     *
     * Both tiers, because section 23.2 is not a tier. At Tier 0 this
     * function does nothing else at all, and this is the whole of what
     * it is for there.
     */
    if (!buf_may_deliver(client->t_proc, server->t_proc, m->buf,
      m->buf_len)) {
        return SYS_ERR_FAULT;
    }

#if SYS_TIER == 2
    {
        struct addrspace *to = proc_as(server->t_proc);
        uintptr_t first = m->buf & ~(uintptr_t)PAGE_MASK;
        uintptr_t last = (m->buf + m->buf_len - 1) & ~(uintptr_t)PAGE_MASK;
        uintptr_t sbase, rbase;
        size_t slen, rlen;
        uintptr_t va;

        /*
         * Nothing to do when the two run in the same memory: a server
         * and a client in one process, or two members of one cohort.
         * The page is already where the receiver will look for it and
         * no entry anywhere has to change, which is the whole of what
         * a shared protection domain buys on this path.
         *
         * First, before anything reasons about whose memory this is.
         * A member may describe the domain's memory and not only its
         * own, per buf_may_deliver(), so the question the assertion below
         * asks has no answer here and does not need one: nothing is
         * being moved into anywhere.
         */
        if (proc_as(client->t_proc) == to) {
            return SYS_OK;
        }

        /*
         * Validation refused a buffer the sender has no slot for, so
         * anything that reaches here is inside one. It is worth an
         * assertion rather than a comment: the two rules have to agree,
         * and the version where they did not let a program be handed an
         * address in kernel memory that it could not touch.
         */
        if (!proc_slot(client->t_proc, &sbase, &slen) ||
          first < sbase || last >= sbase + slen) {
            panic("ipc: %s sent a buffer outside its own memory",
              client->t_name);
        }

        /*
         * The receiver's own memory may not be lent over. Two processes
         * from one image occupy the same addresses at this tier, which is
         * exactly what an address space each buys, and mapping one of
         * them into the other at the address they share would replace
         * the receiver's own memory with the sender's and then unmap it
         * at the reply. Keeping the address is what makes a buffer cheap
         * and this is what it costs: two copies of one image cannot pass
         * buffers to each other, and are told so rather than corrupted.
         */
        if (proc_slot(server->t_proc, &rbase, &rlen) &&
          first < rbase + rlen && last >= rbase) {
            return SYS_ERR_FAULT;
        }

        /*
         * Nothing may be lent to this client already. A client is
         * blocked from the send until the reply and cannot ask twice,
         * so a record still here is a reply that did not give the
         * memory back, and the pages would stay mapped for good.
         */
        if (client->t_lend_len != 0) {
            panic("ipc: %s still has a buffer lent out", client->t_name);
        }

        client->t_lend_va = first;
        client->t_lend_len = 0;
        client->t_lend_to = server->t_proc;

        for (va = first; va <= last; va += PAGE_SIZE) {
            uintptr_t pa;
            sys_err_t err;

            /*
             * Not mapped means not the sender's to send. The address
             * is inside its slot, because validation said so, and a
             * page of its slot that is not there is one it has
             * already lent to somebody else: two threads sharing a
             * page and each sending a buffer in it.
             */
            if (md_lookup(proc_as(client->t_proc), va, &pa) != SYS_OK) {
                msg_buf_take_back(client);
                return SYS_ERR_FAULT;
            }

            err = md_map(to, va, pa, PAGE_SIZE, buf_perms(m->buf_flags));

            if (err != SYS_OK) {
                msg_buf_take_back(client);
                return err;
            }

            /*
             * And out of the sender, which is what makes this a move
             * rather than a loan. Section 6.1: the sender loses the
             * right to the memory until the reply arrives, and until
             * this line that was a rule nothing enforced on either
             * tier. A sender with a second thread now faults on its own
             * buffer while the call is outstanding, which is the whole
             * of what Tier 2 is for.
             *
             * Taken away rather than removed, which is how everything
             * here takes memory away: the entry stays and loses the bit
             * that lets user mode in. What that buys is a permission
             * fault rather than a translation fault, and with a pager in
             * the system the difference is the difference between a
             * program touching memory it lent out and a page that is
             * simply not there yet. One of those is a question for a
             * pager and the other is a program with a bug.
             */
            if (md_map(proc_as(client->t_proc), va, pa, PAGE_SIZE,
              VM_READ | VM_WRITE) != SYS_OK) {
                panic("ipc: %s cannot have its own page taken away",
                  client->t_name);
            }

            client->t_lend_len += PAGE_SIZE;
        }

        /*
         * The receiver may already be running in this space, and on the
         * other path it is about to be. Either way the entries are new
         * and nothing cached can contradict them, but a negative entry
         * from an earlier fault on the same address can, and that is
         * exactly the address a server is about to touch.
         *
         * By address, and one call covers both spaces. The page went
         * into the receiver at the address it had in the sender and
         * was taken away from the sender at that same address, and the
         * call names no identifier, so removing that address removes
         * both entries.
         *
         * This used to say that a port with address space identifiers
         * would need two calls here rather than one, and that the
         * count of one was a cost this was not paying. Both ports have
         * identifiers since M19 and this is still one call, because
         * the by-address form on both of them names no space and
         * reaches every one. M19 stage six then asked whether two
         * calls that do name the two spaces would be cheaper anyway,
         * and they are not: two of them cost about twice one on both,
         * and one of them costs about what one broad call costs.
         *
         * Throwing the whole cache away instead was correct and was
         * also every other translation in the system, most of which
         * had not moved.
         */
        for (va = first; va <= last; va += PAGE_SIZE) {
            md_tlb_invalidate(va);
        }

        vm_charge_pages((uint32_t)(client->t_lend_len / PAGE_SIZE));
    }
#else
    (void)server;
    (void)client;
#endif

    return SYS_OK;
}

/*
 * Rendezvous taken, for section 25.7's measurement. One per rendezvous
 * that happens, counted by whichever side completes it, so a request
 * that crosses three messages counts three and a compound counts one.
 *
 * A send that is refused before it reaches a receiver does not count.
 * It was counted on entry once, which made a failed send look like a
 * round trip in a measurement whose whole purpose is comparing counts
 * of round trips.
 */
uint64_t channel_rendezvous;

/* Clients released because the thread serving them ended. */
uint64_t channel_stale_clients;

/*
 * How many sends gave up at their deadline, told apart by which of
 * section 12.1's two states they were in. The section says the two
 * differ and that the difference is what the server sees, so counting
 * them together would report the thing the section says not to.
 */
uint64_t channel_gave_up_queued;
uint64_t channel_gave_up_working;

/*
 * Whether this sender may describe this memory to this receiver.
 *
 * The ordinary answer is that it must be the sender's own, per section
 * 23.2: what arrives from a program is a number, and a number naming
 * another program's memory would be acted on for that program if
 * nothing checked.
 *
 * Two members of one cohort are the exception, and it is the whole of
 * what section 18 buys on this path. They are one protection domain, so
 * the receiver can already reach everything the sender can, and the
 * kernel maps nothing and dereferences nothing: at Tier 2 the hand-over
 * sees one address space at both ends and does no work, and at Tier 0
 * there was never any work to do. No authority is created by allowing
 * it, because none is being moved.
 *
 * The test is the cohort and not the address space, and that difference
 * is the reason a cohort is a named thing at Tier 0 rather than
 * nothing. At Tier 0 every process shares an address space, so a rule
 * asking about the space would let any program describe any other
 * program's memory and would undo what M4 built. Asking about the
 * cohort gives the same answer at both tiers, which is what the two
 * tiers are for.
 *
 * What this does not check is that the sender can reach the address.
 * It does not have to: the receiver is in the same space, so a bad
 * address is a fault in the member that touches it, exactly as it
 * would be if one member had passed the number to itself.
 *
 * The rule is asked in two places because a channel does not know who
 * receives on it: it was created by whoever assembled the system and
 * the receive right was moved to a server afterwards. So there are two
 * functions rather than one with a receiver that is sometimes absent.
 * An argument meaning "not known yet" in one caller and "no process"
 * in another is one value standing for two questions, and this file
 * has been bitten by that shape more than once.
 *
 * buf_may_deliver() is the rule. buf_may_send() is the most that can
 * be decided before there is a receiver, and it must never be stricter
 * than the rule, or a legitimate send would be refused before anyone
 * could say it was legitimate.
 */
static bool
buf_may_send(struct proc *from, uintptr_t va, size_t len)
{
    return proc_owns(from, va, len) || from->p_cohort != NULL;
}

static bool
buf_may_deliver(struct proc *from, struct proc *to, uintptr_t va, size_t len)
{
    if (proc_owns(from, va, len)) {
        return true;
    }

    return from->p_cohort != NULL && from->p_cohort == to->p_cohort;
}

static sys_err_t
send_inner(struct channel *ch, uint64_t cookie, const struct sys_msg *req,
  struct sys_msg *reply, uint64_t deadline)
{
    struct thread *self = thread_current();
    struct thread *server;
    intr_state_t s;
    sys_err_t err;

    /*
     * The buffer a caller describes has to be the caller's own memory.
     * Section 23.2: what arrives from a program is a number, and a number
     * that names another program's memory would be read on that program's
     * behalf if nothing checked. This is a refusal, not a fault: the
     * caller gets an error and goes on running.
     *
     * Both tiers check. Tier 2 would catch a bad address eventually, as a
     * fault in whichever process dereferenced it, and a refusal here is
     * an answer instead of a corpse.
     */
    if (req->buf_len > 0 && !buf_may_send(self->t_proc,
      req->buf, req->buf_len)) {
        return SYS_ERR_FAULT;
    }

    /*
     * A buffer has to name the access it grants. Section 6.1 has the
     * flags say what the receiver may do with the memory, so a buffer
     * that grants nothing is a description of nothing, and there is no
     * mapping that means it: on one port it would become a page nobody
     * may touch, and on the other the encoding for "no access" is the
     * encoding for "this is a page table", which would have the hardware
     * walk into a page whose contents the sender chose.
     */
    if (req->buf_len > 0 &&
      (req->buf_flags & (SYS_BUF_READ | SYS_BUF_WRITE)) == 0) {
        return SYS_ERR_INVALID_ARG;
    }

    s = md_intr_disable();

    self->t_cookie = cookie;
    /*
     * Into this thread's staging message while its own memory is
     * the memory that is mapped. Everything after this reads the
     * copy, which is reachable from every address space.
     */
    msg_copy(&self->t_stage, req);
    self->t_msg = &self->t_stage;
    self->t_reply = reply;
    self->t_replied = false;
    self->t_waitres = SYS_ERR_INTERRUPTED;

    server = list_pop(&ch->c_receivers);
    if (server != NULL) {
        /*
         * A server was already waiting, so the rendezvous completes now.
         * It takes the message and this thread stays blocked for the
         * reply, which is what makes the blocked sender the queue slot.
         */
        msg_copy(&server->t_stage, &self->t_stage);
        vm_charge_begin();
        err = msg_buf_hand_over(server, self, &server->t_stage);
        vm_charge_end();
        if (err != SYS_OK) {
            /*
             * The receiver has not been touched yet, so putting it
             * back on the list leaves the channel as it was and the
             * caller learns that its own buffer could not be handed
             * over. Doing this after the receiver had been readied
             * would be a receiver woken with no message.
             */
            server->t_snext = ch->c_receivers;
            ch->c_receivers = server;
            self->t_msg = NULL;
            self->t_reply = NULL;
            md_intr_restore(s);
            return err;
        }
        server->t_client = thread_id(self);
        server->t_cookie = cookie;
        server->t_donor = self;
        self->t_donee = server;
        server->t_blocked_on = NULL;
        server->t_waitres = SYS_OK;

        server->t_wait = THREAD_WAIT_NONE;
        serving_begin(ch, server);
        donate(server, self->t_prio);

        /*
         * Readied without preempting, because this thread is about to
         * block and the two are one step. A server that outranks its
         * client would otherwise run before the client had blocked, reply
         * to a thread the preemption had already queued, and panic on
         * readying a thread that is already runnable.
         */
        sched_ready_nopreempt(server);

        /*
         * A server was already waiting, so this never queued: it is
         * REPLY-blocked from the outset and section 12.1 says the two
         * states are told apart precisely here.
         */
        channel_rendezvous++;
        self->t_wait = THREAD_WAIT_REPLY;
    } else {
        /*
         * Nobody is at a receive point, so this thread would queue.
         * Unless it was not willing to wait: section 14.3 makes
         * SYS_DEADLINE_NOW a poll, and a poll that finds no server has
         * its answer. Nothing was queued and nothing was delivered,
         * which is the case section 12.1 calls clean.
         *
         * Before the raise rather than after, so that there is nothing
         * to undo. Asked here rather than at the top of the function,
         * because a send that finds a server waiting completes without
         * blocking and a deadline has nothing to say about it.
         */
        if (deadline <= time_now()) {
            self->t_msg = NULL;
            self->t_reply = NULL;
            md_intr_restore(s);
            return SYS_ERR_TIMEOUT;
        }

        /*
         * Section 12.3 has the priority-ordered queue serve it, and
         * that is only half: whoever is inside a request on this
         * channel may be blocked further down, and everything below is
         * running at the priority of the client it is serving rather
         * than of this one.
         */
        err = chain_raise(ch, self->t_prio);
        if (err != SYS_OK) {
            self->t_msg = NULL;
            self->t_reply = NULL;
            md_intr_restore(s);
            return err;
        }

        sender_insert(ch, self);
        self->t_wait = THREAD_WAIT_SEND;

        /*
         * And the depth of section 12.1, per M18 stage six. The insert
         * happened first and is not conditional: a send joins the
         * queue and then the queue gives up whatever it cannot hold,
         * which is one rule rather than a "would this fit" asked
         * before every insert and answered again after it.
         *
         * The lowest-priority sender goes, found by looking rather
         * than taken from the end: see sender_survey for why the end is
         * not always the lowest. Usually it is this thread; when
         * somebody who outranks the queue arrives it is whoever was
         * last in line at the lowest priority there.
         *
         * A loop rather than one, so that a depth lowered under a
         * queue that was already longer converges rather than staying
         * over for as many sends as it is over by.
         */
        while (ch->c_bounded) {
            struct thread *low;

            if (sender_survey(ch, &low) <= ch->c_depth) {
                break;
            }

            (void)list_remove(&ch->c_senders, low);

            if (low == self) {
                /*
                 * Never blocked, so there is nothing to wake: this is
                 * an answer, returned the way the poll above returns
                 * one. What it does not do is undo the raise, because
                 * a queued sender that gives up at its deadline does
                 * not either, and a priority this send lent to the
                 * chain is not this send's to take back on its way
                 * out.
                 */
                self->t_msg = NULL;
                self->t_reply = NULL;
                self->t_wait = THREAD_WAIT_NONE;
                self->t_snext = NULL;
                channel_senders_refused++;
                md_intr_restore(s);

                return SYS_ERR_WOULDBLOCK;
            }

            /*
             * Readied here and not deferred, unlike the other places
             * this file releases a run of threads. Whoever goes is at
             * or below the priority of the thread doing the releasing,
             * so nothing here can preempt, and deferring would put a
             * dispatch point at the undefer: this thread is on the
             * queue by now with nothing yet saying it is blocked on
             * it, and being switched away from in that state is a
             * thread a server could take a message from while it is
             * still runnable.
             */
            channel_senders_displaced++;
            release_blocked(low, SYS_ERR_WOULDBLOCK);
        }

    }
    trace_wait(self, self->t_wait);

    self->t_blocked_on = ch;
    self->t_unwait = channel_unwait_thread;

    /*
     * And the deadline it said it would wait until, whichever of the
     * two states it is in. Section 12.1 has them give up differently
     * and channel_unwait_thread is where that difference lives: a
     * queued sender is taken off the queue and nobody ever knew, and a
     * client whose message a server holds abandons work in progress and
     * the server finds out when it replies.
     */
    if (deadline != SYS_DEADLINE_NEVER) {
        time_sleep_until(self, deadline);
    }

    {
        /*
         * The round trip as the client sees it: from giving up the
         * core to having its reply. Everything the server does is
         * inside this, which is the point.
         */
        uint64_t started = ipc_round_begin();
        uint64_t points = preempt_points();
        uint32_t core = cpu_self()->cp_index;

        sched_block();
        ipc_round_end(started, points, core);
    }

    /*
     * Whichever happened first, the other is stale: a server that took
     * the message leaves a deadline registered, and a deadline that
     * fired leaves nothing on the queue because the wake took it off.
     * Withdrawn here rather than by whoever did the waking, so neither
     * has to know about the other, which is how timeline_wait does it.
     */
    time_sleep_cancel(self);

    err = self->t_waitres;

    /*
     * And what the timer could not do from an interrupt. A client that
     * gave up while a server held its message has a buffer lent out and
     * a priority donated, and both come back here, where there is a
     * thread to charge the work to and the client is the thread doing
     * it. Section 12.1: it abandons work in progress, and the server
     * finds out when its reply is refused.
     */
    if (err == SYS_ERR_TIMEOUT && self->t_wait == THREAD_WAIT_REPLY) {
        if (self->t_donee != NULL) {
            if (self->t_donee->t_donor == self) {
                undonate(self->t_donee);
            }
            self->t_donee = NULL;
        }

        vm_charge_begin();
        msg_buf_take_back(self);
        vm_charge_end();

    }

    /*
     * Nothing may still be lent out. A reply gives the buffer back
     * before it releases this thread, and until M15 a reply was the
     * only thing that released it.
     *
     * A deadline is the other thing now, and a SEND-blocked client that
     * gives up has lent nothing: the hand-over happens when a server
     * takes the message, and nobody took it. A REPLY-blocked client has
     * lent something and this is where that would be found, which is
     * stage four. The alternative to finding it is a program running
     * again with a hole where its memory was.
     */
    if (self->t_lend_len != 0) {
        panic("ipc: %s ran again with its buffer still lent out",
          self->t_name);
    }


    /*
     * The wait is over and this thread's own fields are put back
     * before anything else can see them, which is what still needs
     * interrupts off. A reply arriving after this finds `t_reply`
     * cleared and is refused as stale, which is the right answer: it
     * would be a second reply to a request already answered.
     */
    {
        bool replied = self->t_replied;

        self->t_wait = THREAD_WAIT_NONE;
        self->t_msg = NULL;
        self->t_reply = NULL;

        /*
         * And the region that began in whoever dispatched to this
         * thread ends here, before the copy rather than after it.
         *
         * What is left to do is read this thread's staging message and
         * write the caller's buffer. The staging message was filled by
         * the replier before it released this thread and nothing will
         * write it again; the caller's buffer is this thread's own. A
         * copy between two things only this thread can reach does not
         * need interrupts off, and it was the last thing in the region
         * that did not.
         */
        md_intr_restore(s);

        /*
         * Whether a reply happened is the question, not whether it
         * succeeded. A handle that failed to move is a reply with an
         * error in it and a payload the server still wrote, and the
         * first version of this dropped the payload on the floor for
         * every error the reply path can report.
         */
        if (replied) {
            msg_copy(reply, &self->t_stage);
        }
    }

    return err;
}

/*
 * Say, once, that user pulses were dropped since the last receive.
 *
 * On any receive and not only on a pulse, which section 13.1 asks for:
 * the flag is what the receiver observes on its next receive, and a
 * server that took a message in between would otherwise learn late or
 * not at all.
 *
 * Reported once and then forgotten. The question it answers is whether
 * anything was missed, not how much: a count would be a number a
 * receiver could do nothing with, and leaving the flag set would make
 * every later receive claim a loss that had already been reported.
 */
static sys_err_t recv_inner(struct channel *ch, struct sys_msg *out,
  sys_thread_id_t *out_peer, uint64_t *out_cookie);

static bool
pulse_take_lost(struct channel *ch)
{
    bool lost = ch->c_pulse_lost;

    ch->c_pulse_lost = false;

    return lost;
}

/*
 * Take the oldest pulse, if there is one. Called with interrupts off.
 */
static bool
pulse_take(struct channel *ch, struct sys_msg *out)
{
    const sys_pulse_t *p;

    if (ch->c_pulse_count == 0) {
        return false;
    }

    p = &ch->c_pulses[ch->c_pulse_head];

    memset(out, 0, sizeof(*out));
    out->handle = SYS_HANDLE_INVALID;
    out->flags = SYS_MSG_IS_PULSE;
    out->inline_len = sizeof(*p);
    memcpy(out->inline_data, p, sizeof(*p));

    if (p->code >= SYS_PULSE_USER_BASE) {
        ch->c_pulse_users--;
    }
    ch->c_pulse_head = (ch->c_pulse_head + 1) % SYS_PULSE_SLOTS;
    ch->c_pulse_count--;

    /*
     * Reported once and then forgotten. The question it answers is
     * whether anything was missed, not how much: a count would be a
     * number a receiver could do nothing with, and leaving the flag set
     * would make every later receive claim a loss that had already been
     * reported.
     */
    if (pulse_take_lost(ch)) {
        out->flags |= SYS_MSG_PULSE_LOST;
    }

    return true;
}

static sys_err_t
recv_inner(struct channel *ch, struct sys_msg *out,
  sys_thread_id_t *out_peer, uint64_t *out_cookie)
{
    struct thread *self = thread_current();
    struct thread *client;
    intr_state_t s;
    bool lost;

    s = md_intr_disable();

    /*
     * A loop, and not because anything here retries. A receiver woken
     * for a pulse can find it gone, because the wake and the taking are
     * two steps and another receiver can run between them, and a
     * receiver that then read its staging message would be handed
     * whatever the last message on this channel happened to be. So the
     * question is asked again rather than assumed.
     */
    for (;;) {
        /*
         * A pulse first, per section 13.1: one receive point for both,
         * and a notification about something that has already happened
         * should not wait behind work. The queue is bounded and drains,
         * so this cannot starve messages: there are at most
         * SYS_PULSE_SLOTS of them and nothing refills it faster than
         * this empties it.
         */
        if (pulse_take(ch, out)) {
            if (out_peer != NULL) {
                *out_peer = 0;
            }
            if (out_cookie != NULL) {
                *out_cookie = 0;
            }
            md_intr_restore(s);
            return SYS_OK;
        }

        client = list_pop(&ch->c_senders);
        if (client != NULL) {
            sys_err_t lend;

            msg_copy(out, &client->t_stage);
            vm_charge_begin();
            lend = msg_buf_hand_over(self, client, out);
            vm_charge_end();
            if (lend != SYS_OK) {
                /*
                 * The buffer could not be put where this thread could
                 * reach it, so there is no message to serve. The
                 * client is told, rather than left blocked on a
                 * rendezvous that did not happen, and this thread goes
                 * on to wait like one that found nothing queued.
                 *
                 * Not replied to, which is the field that says whether
                 * there is a reply to copy out. Setting it here would
                 * hand the client its own request back, dressed as an
                 * answer to it.
                 */
                client->t_replied = false;
                client->t_waitres = lend;
                client->t_blocked_on = NULL;
                client->t_wait = THREAD_WAIT_NONE;
                sched_ready(client);
                client = NULL;
            }
        }
        if (client != NULL) {
            channel_rendezvous++;
            self->t_client = thread_id(client);
            self->t_cookie = client->t_cookie;
            self->t_donor = client;
            client->t_donee = self;

            serving_begin(ch, self);
            donate(self, client->t_prio);

            /*
             * The client moves from SEND-blocked to REPLY-blocked
             * without running. It was queued and is now being served.
             */
            client->t_wait = THREAD_WAIT_REPLY;
            trace_wait(client, THREAD_WAIT_REPLY);

            self->t_wait = THREAD_WAIT_NONE;

            if (pulse_take_lost(ch)) {
                out->flags |= SYS_MSG_PULSE_LOST;
            }
            *out_peer = self->t_client;
            *out_cookie = self->t_cookie;
            md_intr_restore(s);

            return SYS_OK;
        }

        /*
         * Nothing records where the message will go: a sender writes
         * this thread's staging message, and this thread copies it out
         * when it runs again and its own memory is mapped.
         *
         * The client id is cleared first, because after the block it is
         * what says which kind of wake this was. A sender fills it in;
         * a pulse does not, and leaves it zero.
         */
        self->t_msg = &self->t_stage;
        self->t_client = 0;
        self->t_snext = ch->c_receivers;
        ch->c_receivers = self;

        self->t_wait = THREAD_WAIT_RECV;
        trace_wait(self, THREAD_WAIT_RECV);

        self->t_blocked_on = ch;
        self->t_unwait = channel_unwait_thread;
        sched_block();

        /*
         * Running again, which for a server that was donated to is the
         * moment section 12.3's mechanism has finished doing anything.
         */
        ipc_donate_arrived(self);

        /*
         * Whether pulses were dropped is read and cleared here, with
         * interrupts still off, because it is the channel's and a
         * pulse arriving from a handler sets it. Reading it after the
         * restore would lose one that arrived in between: the send
         * side sets the flag under the same exclusion, and taking it
         * outside was a hole this stage opened and its review found.
         *
         * The bit it turns into is applied to the caller's message
         * after the copy, because the copy overwrites the flags.
         */
        lost = self->t_client != 0 && pulse_take_lost(ch);

        /*
         * The region this thread woke into ends here, and not after
         * the copy below.
         *
         * It began in the sender's `channel_send`, ran across the
         * context switch, and was closing at whichever restore this
         * function reached. Everything from here on is this thread's
         * own memory: its staging message, which the sender filled
         * before it blocked and will not touch again while it waits
         * for a reply, and the caller's output buffer, which is this
         * thread's. Neither is shared with anything that could run in
         * between, so neither needs interrupts off.
         *
         * What did need them off is over: the rendezvous is complete,
         * this thread is running, and its wait state is settled.
         */
        md_intr_restore(s);

        if (self->t_waitres != SYS_OK) {
            *out_peer = self->t_client;
            *out_cookie = self->t_cookie;

            return self->t_waitres;
        }

        /*
         * A sender filled this thread's staging message while this
         * thread was blocked. Copy it out now that this thread's memory
         * is what is mapped.
         */
        if (self->t_client != 0) {
            msg_copy(out, &self->t_stage);
            if (lost) {
                out->flags |= SYS_MSG_PULSE_LOST;
            }
            *out_peer = self->t_client;
            *out_cookie = self->t_cookie;

            return SYS_OK;
        }

        /*
         * Woken for a pulse. Round again to take it, or to wait anew,
         * with interrupts off as the top of the loop expects.
         */
        s = md_intr_disable();
    }
}

/*
 * And a send is the third way out, per M20 stage seven.
 *
 * Stage four wrapped the receive and the reply and said there were
 * three. The third is a send that leaves without being served: a
 * deadline that expires, a cancellation, an owner that died. The
 * thread comes off the sender queue on all of those, and if nothing
 * names the channel by then it was the last thing keeping the slot.
 * Without this a channel whose holders all went while somebody was
 * queued on it is given back when the sender is served and leaked
 * when the sender gives up, which is the case nothing was asking
 * about.
 *
 * Asked on the served path as well as the abandoned one, because the
 * question is the same and one wrapper is cheaper to read than a
 * branch. By identifier for the reason the receive gives: a send
 * blocks, and what it comes back to may not be the channel it left.
 */
/*
 * The kernel lock around a call that came from a kernel thread.
 *
 * Every entry from user mode arrives on a vector that takes the lock
 * on the way in and drops it on the way out, so the body below runs
 * under it. A kernel thread calling the same thing holds nothing,
 * and until M21 stage six's review that is how the boot's checks
 * made these calls: the same operations with the exclusion missing.
 *
 * What it costs is a lost wake. A call that means to block reads the
 * state, finds it not ready, and puts the thread on a list; the core
 * that makes it ready writes the state and walks that list. The two
 * interleave in one order that leaves a thread on a list nobody will
 * look at again, and the deadline is the only thing that ends it.
 *
 * Taken here rather than at each call site, because the rule is
 * about what a call is and not about who happened to make it.
 */

sys_err_t
channel_send(struct channel *ch, uint64_t cookie, const struct sys_msg *req,
  struct sys_msg *reply, uint64_t deadline)
{
    sys_id_t id = ch->c_id;
    intr_state_t s = 0;
    bool held = klock_held();
    sys_err_t err;

    if (!held) {
        s = klock_enter();
    }

    err = send_inner(ch, cookie, req, reply, deadline);

    channel_maybe_free(pool_lookup(channel_pool(), id));

    if (!held) {
        klock_exit(s);
    }

    return err;
}

/*
 * And the channel may have nothing left in it, per M20 stage four.
 *
 * A receive is one of the three ways a thread leaves a channel: it
 * takes a sender off the queue, and it takes this thread off the
 * receivers. If nothing names the channel by then, the thread that
 * just left was the last thing keeping the slot.
 *
 * Around the whole of it rather than at each of its returns, because
 * there are five of those and the question is the same at all of
 * them. The caller is finished with the channel either way: the
 * operation layer reads the message and the error and never the
 * channel.
 *
 * By identifier and not by pointer, which is not caution about the
 * common case. A receive blocks, the lock is put down across the
 * switch, and something else can take the last hold and free this
 * channel while this thread is away. Coming back and reading
 * c_holds through the old pointer would read whatever occupies the
 * slot now, and finding zero there would free a channel somebody is
 * using. The generation the pool keeps is what answers this: it says
 * the slot has moved on, and a lookup returns nothing rather than
 * returning the wrong thing.
 */
sys_err_t
channel_recv(struct channel *ch, struct sys_msg *out,
  sys_thread_id_t *out_peer, uint64_t *out_cookie)
{
    sys_id_t id = ch->c_id;
    intr_state_t s = 0;
    bool held = klock_held();
    sys_err_t err;

    if (!held) {
        s = klock_enter();
    }

    err = recv_inner(ch, out, out_peer, out_cookie);

    channel_maybe_free(pool_lookup(channel_pool(), id));

    if (!held) {
        klock_exit(s);
    }

    return err;
}

/*
 * Reply, and stop running at the client's priority.
 *
 * A reply to a client that has died is refused rather than delivered,
 * because the id carries a generation and the slot it names is no longer
 * the thread that sent the request.
 */
static sys_err_t
reply_inner(sys_thread_id_t peer, const struct sys_msg *reply)
{
    struct thread *self = thread_current();
    struct thread *client;
    intr_state_t s;
    sys_err_t err;

    s = md_intr_disable();

    client = thread_by_id(peer);
    if (client == NULL || client->t_reply == NULL) {
        /*
         * The client is gone, and this server is still on the
         * channel's list of threads inside a request. Nothing else
         * takes it off: a normal reply does it below, and this returns
         * before reaching there. chain_raise walks that list to donate
         * a sender's priority down it, so a server left on it is a
         * thread that gets raised for work it is no longer doing, and
         * the walk follows what it is blocked on into somebody else's
         * chain.
         *
         * Only when the client named is the one this server was
         * holding. A reply to an identifier that was never its own is
         * refused the same way and has nothing to do with the request
         * it does have.
         */
        if (self->t_client == peer) {
            serving_end(self);
            self->t_client = 0;
        }

        md_intr_restore(s);
        return SYS_ERR_STALE;
    }

    /*
     * The buffer goes home first. Section 6.1 returns the right to the
     * sender when the reply arrives, and the client is about to be made
     * runnable: after that point there is a thread that may touch memory
     * another process still has mapped.
     */
    vm_charge_begin();
    msg_buf_take_back(client);
    vm_charge_end();

    /*
     * Section 7.4: a handle travels by move. The value changes across the
     * boundary because a handle means something only in the table holding
     * it, and the sender slot is invalidated rather than merely unused, so
     * presenting the old value afterwards is refused rather than reaching
     * the object.
     */
    msg_copy(&client->t_stage, reply);
    client->t_stage.handle = SYS_ID_INVALID;

    /*
     * A reply carries no buffer. Section 6.1: a server that handed back
     * memory of its own could not tell when the client had finished with
     * it and so could never reclaim it, which is why a large reply writes
     * into the buffer the client supplied in the request instead.
     */
    client->t_stage.buf = 0;
    client->t_stage.buf_len = 0;
    client->t_stage.buf_flags = 0;

    err = SYS_OK;
    if (reply->handle != SYS_ID_INVALID) {
        err = handle_move(&self->t_proc->p_handles,
          &client->t_proc->p_handles, reply->handle,
          &client->t_stage.handle);
    }

    /*
     * The client is released whether or not the handle made it across.
     * Returning early on a failed transfer would leave it blocked on a
     * reply that has already been sent, which is a worse outcome than
     * telling both sides the transfer failed.
     */
    client->t_replied = true;
    client->t_waitres = err;
    client->t_blocked_on = NULL;
    client->t_wait = THREAD_WAIT_NONE;

    if (self->t_donor == client) {
        undonate(self);
    }
    client->t_donee = NULL;
    serving_end(self);
    self->t_client = 0;

    /*
     * The client is made runnable here and dispatched to below, which
     * used to be one step and is two so that they are two
     * interrupts-off regions instead of one.
     *
     * Section 13.3's first term was measured as beginning here and
     * ending in the client's own `channel_send`, which is to say it
     * covered this reply's work, the context switch, and the client
     * coming back. A quarter to a half of it was the work above.
     *
     * What that work needs interrupts off for is that the client's
     * state is being taken apart and put back together: it is blocked
     * on us, its buffer right is being returned, a handle may be
     * moving into its table, and a donation may be being unwound. A
     * thread that saw any of that half done would see a client that is
     * neither blocked nor runnable. None of it is safe to interleave.
     *
     * By the time the deferral is lifted, none of that is true any
     * more. The client is released and enqueued, this thread has given
     * up what it was holding on the client's behalf, and the only
     * thing left is the choice of which thread runs next.
     *
     * The deferral is what moves that choice out of the region.
     * Without it `sched_ready` dispatches where it stands, which is
     * inside the region this is trying to end, and the split does not
     * happen at all.
     *
     * Between the restore and the lift nothing can preempt this
     * thread, because the deferral suppresses dispatch on this core
     * and that is what a deferral is. An interrupt taken in that gap
     * is taken at once, which is the whole gain; the thread it makes
     * runnable waits for the lift, which is the next instruction.
     */
    sched_defer();
    sched_ready(client);

    md_intr_restore(s);

    /*
     * And the switch, in a region of its own that begins here rather
     * than at the top of this function.
     */
    sched_undefer();

    return err;
}

/*
 * And the channel this server was serving on may have nothing left in
 * it, per M20 stage four.
 *
 * A reply is the second of the three ways a thread leaves a channel:
 * it takes this thread off the serving list. The channel has to be
 * read before the reply rather than after, because after it this
 * thread is not serving anything and there is nothing left to ask.
 * Its identifier is what is kept, for the reason the receive above
 * gives: a reply hands the core to the client it woke, and the slot
 * can be gone and taken by the time this thread runs again.
 *
 * A reply that is refused still takes the thread off the list, which
 * is the case the comment inside is about, so the question is asked
 * whatever the answer was.
 */
sys_err_t
channel_reply(sys_thread_id_t peer, const struct sys_msg *reply)
{
    const struct channel *was;
    intr_state_t s = 0;
    bool held = klock_held();
    sys_id_t id;
    sys_err_t err;

    if (!held) {
        s = klock_enter();
    }

    was = thread_current()->t_serve_ch;
    id = was != NULL ? was->c_id : SYS_ID_INVALID;
    err = reply_inner(peer, reply);

    channel_maybe_free(pool_lookup(channel_pool(), id));

    if (!held) {
        klock_exit(s);
    }

    return err;
}

/*
 * Whether any channel still says it belongs to this process.
 *
 * A channel keeps a pointer to its owner and takes no reference for it,
 * so a reaped owner would leave that pointer naming a freed slot, and a
 * reused one would leave it naming somebody else entirely. Nothing in
 * the generation counters helps here: they protect identifiers, and
 * this is a pointer.
 *
 * Not reachable as things stand. There is no operation for making a
 * channel, so every one belongs to a process the kernel built, and the
 * kernel's own process never exits. This exists because that is a fact
 * about who calls channel_create() today rather than a rule, and the
 * check sequence already builds one channel for a process it has
 * finished with.
 *
 * The real answer is teardown: a channel whose owner dies should go,
 * releasing its senders and telling its receivers, which is stage four
 * and stage six. Until then this makes the hazard stop the machine
 * rather than corrupt it quietly.
 */
bool
channel_owned_by(const struct proc *p)
{
    const struct pool *pool = channel_pool();
    uint32_t i;

    for (i = 0; i < pool->count; i++) {
        const struct channel *ch;

        if (!pool->slots[i].inuse) {
            continue;
        }
        ch = pool_lookup(pool, sys_id_make(i, pool->slots[i].gen));
        if (ch != NULL && ch->c_owner == p) {
            return true;
        }
    }

    return false;
}

/*
 * A thread has been cancelled out of a channel operation, per section
 * 10.5. Called with interrupts disabled, before the thread is made
 * runnable again.
 *
 * Two things have to be undone, and only one of them is memory.
 *
 * The buffer comes home. A client cancelled between the rendezvous and
 * the reply has its pages mapped in the server and taken away from
 * itself, and section 6.1 gives them back at the reply. There is not
 * going to be a reply, so they come back here, and the alternative is a
 * program released into a call it never finished with a hole where its
 * memory was. M4 wrote a panic on the far side of the send saying this
 * is where it would be found when cancellation existed.
 *
 * And the reply has nowhere to go. Section 10.5 step 3 says a server
 * that replies anyway is told SYS_ERR_STALE, and what makes that
 * happen is this: the reply path asks whether the client is still
 * waiting for one, and after this it is not. The server keeps its
 * pointer to a live thread and is refused, rather than writing into a
 * thread that has moved on.
 */
static void
channel_thread_release(struct thread *t, bool tell)
{
    struct channel *ch = t->t_blocked_on;

    tell = tell && t->t_donee != NULL;

    /*
     * The priority it lent goes back first. A server serving a client
     * that has been cancelled has no reason to keep running at that
     * client's priority, and nothing else will ever take it away: the
     * reply is the only place a donation was ever undone, and the reply
     * is not coming. Section 12.3 says nothing about this because
     * nothing before cancellation could reach it.
     *
     * Only if the donation is still this thread's. A server that has
     * already moved on to somebody else is at a priority it owes to
     * them, and taking it away would be this thread reaching into a
     * call it is not part of.
     */
    if (t->t_donee != NULL) {
        if (t->t_donee->t_donor == t) {
            undonate(t->t_donee);
        }
        t->t_donee = NULL;

    }

    vm_charge_begin();
    msg_buf_take_back(t);
    vm_charge_end();

    t->t_msg = NULL;
    t->t_reply = NULL;
    t->t_replied = false;

    /*
     * And the server is told, per section 10.5 step 2 and section
     * 12.5. A server is at a receive point and can collect a pulse,
     * where this thread is not and has to be failed instead: that
     * asymmetry is the whole reason both mechanisms exist rather than
     * one. Without it a server learns only when it replies and is
     * refused, and a server that does not check its replies leaks
     * whatever it allocated for the request for good.
     *
     * Last, after everything above. Leaving a pulse can make a server
     * runnable, and a server that outranks whoever is cancelling runs
     * at once: it must not find this thread still looking like a
     * client waiting for a reply, because it would send one and
     * release a thread that is in the middle of being released.
     *
     * The value is the identifier truncated, per section 7.1, not a
     * bare index. A bare index would let the slot be recycled between
     * the enqueue and the collection and have the server match the
     * notice against somebody else's request.
     */
    if (tell && ch != NULL) {
        channel_pulse(ch, SYS_PULSE_DEAD_OWNER,
          SYS_ID_TAG32(thread_id(t)));
    }
}

/*
 * Whether section 12.5 is going to release this client anyway.
 *
 * A process that is going releases everything blocked on the channels
 * it owns, with SYS_ERR_OWNER_DEAD, when its last thread has gone. A
 * client of a thread of that process must be left for that: told STALE
 * here it would be told the server is still there to be asked again,
 * which is exactly what OWNER_DEAD exists to deny.
 *
 * Owned by this process, and not merely served by it. A thread serving
 * on somebody else's channel is not on the list section 12.5 walks, so
 * its client is this function's business after all.
 *
 * Going, by either route: killed, which sets the mark before any thread
 * of it exits, or ending because this is the last thread it has. The
 * count still includes this thread, which has not yet been taken off.
 */
static bool
owner_going(const struct thread *t, const struct channel *ch)
{
    const struct proc *p = t->t_proc;

    if (p == NULL || ch == NULL || ch->c_owner != p) {
        return false;
    }

    return p->p_dying || p->p_nthreads <= 1;
}

/*
 * A thread is ending, so it is no longer inside a request on anything.
 *
 * A server that received a message and exits without replying leaves
 * itself on its channel's list of threads inside a request, and nothing
 * else takes it off: a reply does it, and there is no reply. chain_raise
 * walks that list to donate a queued sender's priority down it, so what
 * is left behind is a pointer to a dead thread whose pool slot the next
 * thread will be given, and the walk would read its priority and what
 * it is blocked on.
 *
 * And the client it was holding is released, with SYS_ERR_STALE. Until
 * M17 it waited until its own deadline, because section 12.5 releases
 * clients when the process dies and one thread of a live server dying
 * is not that. The wait was for nothing either way: the only thread
 * that could have answered is this one. The error says which kind of
 * nothing it was, and a client that reads it knows the server is still
 * there to be asked again, where OWNER_DEAD says it is not.
 *
 * Only the client this thread is actually serving. The identifier
 * carries a generation, so a slot given to something else since is not
 * found, and t_donee is the client's own record of who is serving it:
 * it is cleared by the reply, by a client giving up, and by a
 * cancellation, which are the three ways of stopping being served.
 *
 * And only while that client is still blocked. A deadline that has
 * already fired leaves a thread runnable with its answer written and
 * its fields not yet put back, and releasing it again would enqueue a
 * thread that is on a run queue. thread_wake_timeout reads the state
 * for the same reason from the other side.
 */
void
channel_thread_exited(struct thread *t)
{
    intr_state_t s = md_intr_disable();

    if (t->t_client != 0) {
        struct thread *client = thread_by_id(t->t_client);

        if (client != NULL && client->t_donee == t &&
          client->t_state == THREAD_BLOCKED &&
          client->t_wait == THREAD_WAIT_REPLY &&
          !owner_going(t, client->t_blocked_on)) {
            /*
             * Deferred, for the reason section 12.5's walk defers:
             * releasing a client makes it runnable, and one that
             * outranks this thread would take the core here, in the
             * middle of an exit that has not yet said this thread is
             * over.
             */
            sched_defer();
            channel_stale_clients++;
            release_blocked(client, SYS_ERR_STALE);
            sched_undefer();
        }
        t->t_client = 0;
    }

    serving_end(t);

    md_intr_restore(s);
}


void
channel_thread_cancelled(struct thread *t)
{
    serving_end(t);

    channel_thread_release(t, true);
}

/*
 * Pulses that did not fit, for M18 stage five. Counts rather than
 * flags because these are the boot's, not a channel's: a receiver is
 * told that it missed something and does not care how much, and these
 * are here so that a check can say which way the queue overflowed.
 */
uint64_t channel_pulses_evicted;
uint64_t channel_pulses_dropped;

/*
 * Take the oldest pulse of one kind out of the queue, and say whether
 * there was one. `users` picks a user pulse; otherwise the kind is the
 * hint of M18 stage four, which is the one system pulse a channel can
 * lose for nothing, because what it announces is on the connection and
 * stays there.
 *
 * It need not be at the head: a pulse of another kind can be in front
 * of it, and taking that instead would be one kind displacing another
 * by a longer route. The oldest goes rather than the newest, which is
 * worth stating because it is a choice. A notification is about
 * something that has happened, and the older one is the one whose news
 * has had longest to be overtaken.
 *
 * Called with interrupts off.
 */
static bool
pulse_drop_oldest(struct channel *ch, bool users)
{
    uint32_t i;

    for (i = 0; i < ch->c_pulse_count; i++) {
        uint32_t k = (ch->c_pulse_head + i) % SYS_PULSE_SLOTS;
        bool user = ch->c_pulses[k].code >= SYS_PULSE_USER_BASE;
        uint32_t j;

        if (user != users) {
            continue;
        }
        if (!user && ch->c_pulses[k].code != SYS_PULSE_PEER_GONE) {
            continue;
        }
        for (j = i; j + 1 < ch->c_pulse_count; j++) {
            uint32_t a = (ch->c_pulse_head + j) % SYS_PULSE_SLOTS;
            uint32_t b = (ch->c_pulse_head + j + 1) % SYS_PULSE_SLOTS;

            ch->c_pulses[a] = ch->c_pulses[b];
        }
        ch->c_pulse_count--;
        if (user) {
            ch->c_pulse_users--;
        }
        ch->c_pulse_lost = true;
        channel_pulses_evicted++;

        return true;
    }

    return false;
}

/*
 * Enqueue a pulse, per section 13.1. Never blocks, and never fails for
 * want of room: what a full queue costs is a pulse, not the caller.
 *
 * The rule is one line and the reason is the whole of why pulses are
 * specified at all. User pulses may fill the queue up to the reserved
 * slots and no further; when they are at that limit the oldest user
 * pulse is dropped to make room and the channel remembers that it
 * happened. A system pulse may use the reserved slots, so a process
 * that floods a channel with pulses of its own cannot push a death
 * notice out.
 *
 * What section 13.1 said next was that a dropped death notice leaks a
 * dead client's resources for good, and it was right while the notice
 * was the only record of the death. M18 stage four put that record on
 * the connection, where it stays until the server collects it, so a
 * queue with more outstanding deaths than slots now drops one instead
 * of halting the machine, and says so.
 */
void
channel_pulse(struct channel *ch, uint16_t code, uint32_t value)
{
    intr_state_t s = md_intr_disable();
    bool system = code < SYS_PULSE_USER_BASE;
    uint32_t limit = SYS_PULSE_SLOTS - SYS_PULSE_RESERVED;
    struct thread *waiter;
    uint32_t at;

    if (!system && ch->c_pulse_users >= limit) {
        (void)pulse_drop_oldest(ch, true);
    }

    if (ch->c_pulse_count == SYS_PULSE_SLOTS) {
        /*
         * A full queue drops the pulse it can most afford to lose, and
         * there is an order to that.
         *
         * A user pulse first, which is the reservation of section 13.1
         * honoured here as well as at the cap above: a queue that
         * reached eight with user pulses in it has room an arriving
         * notice is entitled to. Then a hint, which is the one system
         * pulse that costs nothing to lose, because what it announces
         * is on the connection and stays there until the server
         * collects it. Then the arriving pulse, whatever it is.
         *
         * That last case halted the machine until M18 stage five, on
         * section 13.1's argument that losing a notice leaks a dead
         * client's resources for good. Stage four made that untrue for
         * every client that goes away entirely, so the queue stops
         * being something that must not overflow and becomes what it
         * says it is, a fixed number of hints, with the flag below
         * telling the receiver that it missed something.
         *
         * The arriving one goes rather than one already queued. The
         * ones in the queue are the ones the receiver is about to
         * take, and taking a slot from them to say the same kind of
         * thing again would lose news that is already on its way.
         *
         * Nothing is woken on that path. A receiver blocks only on an
         * empty queue, because a receive takes a pulse before it waits,
         * and the pulse that filled the first slot took the receiver
         * off the list. A full queue therefore has nobody waiting on
         * it.
         */
        if (!pulse_drop_oldest(ch, true) && !pulse_drop_oldest(ch, false)) {
            ch->c_pulse_lost = true;
            channel_pulses_dropped++;
            md_intr_restore(s);
            return;
        }
    }

    at = (ch->c_pulse_head + ch->c_pulse_count) % SYS_PULSE_SLOTS;
    ch->c_pulses[at].code = code;
    ch->c_pulses[at]._reserved = 0;
    ch->c_pulses[at].value = value;
    ch->c_pulse_count++;
    if (!system) {
        ch->c_pulse_users++;
    }

    /*
     * And somebody to collect it, if anybody is waiting. A receiver
     * blocked on this channel is woken exactly as a message would wake
     * it, because section 13.1 gives a server one receive point for
     * both and the wait is the same wait.
     */
    waiter = list_pop(&ch->c_receivers);
    if (waiter != NULL) {
        waiter->t_blocked_on = NULL;
        waiter->t_wait = THREAD_WAIT_NONE;
        waiter->t_waitres = SYS_OK;

        /*
         * Nothing is set to say this was a pulse. What tells the
         * receiver apart is the client identifier, which channel_recv
         * clears before it blocks and a sender fills in before it
         * readies anybody, so a wake with it still clear is a wake
         * that no sender caused. Clearing it here as well would be a
         * second place keeping the same invariant, which is one place
         * too many.
         */
        sched_ready(waiter);
    }

    md_intr_restore(s);
}


/*
 * Release one client blocked on a server that is gone, with the error
 * that says which way it went. Interrupts are already off.
 */
static void channel_thread_release(struct thread *t, bool tell);

static void
release_blocked(struct thread *t, sys_err_t err)
{
    /*
     * What this thread is serving is left where it is, and that is a
     * correction. This used to call serving_end on it, with a comment
     * about a dead server left on a list: the thread being released
     * here is a client, not a server, and the only serving link it can
     * have belongs to a different call that it still owes an answer
     * to. Taking it off that list is reaching into somebody else's
     * request, and what it costs is the chain walk of section 12.3,
     * which can no longer see the middle of a chain in order to raise
     * it. A client released by its own deadline keeps that link, and
     * these two have no reason to differ.
     */

    /*
     * Its buffer first, for the reason cancellation has: the pages are
     * in the server and gone from here, the reply that would have given
     * them back is not coming, and a thread released into its own
     * memory with a hole in it is worse than the wait it was in.
     *
     * And no notice. The thread that would be told is the one that has
     * gone, so there is nobody to tell: leaving a death notice on a
     * channel whose owner has terminated is a pulse nothing will ever
     * collect, and enough of them would fill the slots reserved for the
     * notices that do have a reader.
     */
    channel_thread_release(t, false);

    t->t_waitres = err;
    t->t_blocked_on = NULL;
    t->t_wait = THREAD_WAIT_NONE;
    sched_ready(t);
}

/*
 * A process that owned channels has terminated, per section 12.5.
 *
 * A client blocked in send is not at a receive point and cannot collect
 * a pulse, so it is released with an error instead. That is the whole
 * of why the two directions need different mechanisms: the party that
 * can be told is told, and the party that cannot is failed.
 *
 * Both blocked states, which the section is explicit about. A queued
 * sender is waiting for a server that will never take it, and a
 * reply-blocked client is waiting for a reply that will never come; the
 * first is on the channel's sender list and the second is not on any
 * list at all, which is why this walks the threads for it.
 *
 * At termination and not at reaping. A process that has exited still
 * holds its slot for as long as anything can ask about it, and leaving
 * its clients blocked for that long would make how promptly they are
 * released depend on when an unrelated supervisor let go of a handle.
 */
void
channel_owner_died(struct proc *p)
{
    const struct pool *chans = channel_pool();
    struct pool *threads = thread_pool();
    intr_state_t s = md_intr_disable();
    uint32_t i;

    /*
     * Deferred, for the reason cancellation is: releasing a client
     * makes it runnable, one that outranks the thread doing the
     * releasing would run at once, and the walk below is not finished.
     * A run of readies is one step.
     */
    sched_defer();

    for (i = 0; i < chans->count; i++) {
        struct channel *ch;
        struct thread *t;
        uint32_t j;

        if (!chans->slots[i].inuse) {
            continue;
        }
        ch = pool_lookup(chans, sys_id_make(i, chans->slots[i].gen));
        if (ch == NULL || ch->c_owner != p) {
            continue;
        }

        while ((t = list_pop(&ch->c_senders)) != NULL) {
            release_blocked(t, SYS_ERR_OWNER_DEAD);
        }

        for (j = 0; j < threads->count; j++) {
            if (!threads->slots[j].inuse) {
                continue;
            }
            t = pool_lookup(threads, sys_id_make(j, threads->slots[j].gen));
            if (t == NULL || t->t_blocked_on != ch ||
              t->t_wait != THREAD_WAIT_REPLY) {
                continue;
            }
            release_blocked(t, SYS_ERR_OWNER_DEAD);
        }

        /*
         * And it collects no more departed peers. A connection kept
         * back for a server to collect is kept for a server, and there
         * is no longer one: holding them now would spend the
         * connection pool on an answer nobody will ask for.
         */
        if (ch->c_reaping) {
            struct connection *cn, *next;

            ch->c_reaping = false;
            for (cn = ch->c_conns; cn != NULL; cn = next) {
                next = cn->cn_next;
                if (cn->cn_gone && connection_unheld(cn)) {
                    connection_destroy(cn);
                }
            }
        }

        /*
         * And the channel stops naming a process that has gone. M6
         * left this pointer taking no reference and M7 stage one
         * guarded a reap against it, because a reaped owner would
         * leave it naming a freed slot and a reused one would leave it
         * naming somebody else. Clearing it here is what that guard
         * was waiting for: the pointer is only interesting while the
         * owner is alive, and this is the moment it stops being.
         *
         * The channel itself stays. Anything still holding a receive
         * handle can still receive on it, and tearing one down with
         * its owner is section 18.2's teardown, which is stage six.
         */
        ch->c_owner = NULL;
    }

    sched_undefer();
    md_intr_restore(s);
}

void
channel_set_owner(struct channel *ch, struct proc *p)
{
    ch->c_owner = p;
}

struct proc *
channel_owner(const struct channel *ch)
{
    return ch->c_owner;
}

/*
 * A thread has taken a message from this channel and owes a reply.
 *
 * Section 12.3 asks for a link from a blocked sender to whoever is
 * serving it, and this is where it is made. Until it existed the bound
 * on the chain and the deadlock error described a mechanism that was
 * not there.
 */
static void
serving_begin(struct channel *ch, struct thread *t)
{
    /*
     * Already on this list, and putting it on again made the list
     * point at itself.
     *
     * Section 19.3 says a server thread serves one request at a time
     * and nothing enforces it, which was known. What was not known is
     * what happens when the thread takes the second request on the
     * same channel it is already serving: the line below would read
     * this thread out of c_serving and write it back as its own
     * successor, and the list stops ending. M20 stage two's walk found
     * it in a paced session, where the console driver does exactly
     * that when a line is typed at it.
     *
     * The thread is already recorded as being inside a request on this
     * channel, which is what the list is for, so recording it twice
     * was never going to say anything a walker could use. Refusing the
     * second request is section 19.3's business and is not done here.
     */
    if (t->t_serve_ch == ch) {
        return;
    }

    t->t_serve_ch = ch;
    t->t_serve_next = ch->c_serving;
    ch->c_serving = t;
}


static void
serving_end(struct thread *t)
{
    struct channel *ch = t->t_serve_ch;
    struct thread **link;

    if (ch == NULL) {
        return;
    }

    for (link = &ch->c_serving; *link != NULL; link = &(*link)->t_serve_next) {
        if (*link == t) {
            *link = t->t_serve_next;
            break;
        }
    }

    t->t_serve_ch = NULL;
    t->t_serve_next = NULL;
}

/*
 * Raise everyone between this sender and the bottom of the chain it is
 * waiting on, per section 12.3.
 *
 * A client that queues on a busy server used to raise nobody. The
 * section said there was no thread whose promotion would help, and
 * there is one: the server is inside a request of its own and may be
 * blocked sending further down, and everything below it is running at
 * the priority of whoever it is serving rather than of whoever is
 * waiting.
 *
 * The walk is bounded because the bound is the point. A chain longer
 * than SYS_PRIO_CHAIN_MAX is refused, and so is a cycle, which is the
 * same thing seen from the inside: following it never ends and the
 * bound is what stops the walk rather than what the walk finds.
 */
/*
 * How many threads a chain walk has actually raised, and how deep the
 * deepest walk went. A walk that refuses cycles and never raises
 * anybody is a deadlock detector rather than the inheritance section
 * 12.3 describes, and the two are worth telling apart.
 */
static uint64_t chain_raised;
static uint64_t chain_deepest;

uint64_t
channel_chain_raised(uint64_t *deepest)
{
    *deepest = chain_deepest;

    return chain_raised;
}

static sys_err_t
chain_raise(struct channel *ch, uint8_t prio)
{
    uint32_t depth;

    for (depth = 0; depth < SYS_PRIO_CHAIN_MAX; depth++) {
        struct thread *server = ch->c_serving;

        if (server == NULL) {
            /*
             * Nobody is inside a request on this channel, so the queue
             * being priority-ordered is the whole of what this sender
             * needs. That is the case section 12.3 already described.
             */
            return SYS_OK;
        }

        /*
         * Raised only if it is blocked. A runnable thread is on some
         * core's queue, and moving it between another core's queues is
         * that core's business, per section 11.4.
         */
        if (depth > chain_deepest) {
            chain_deepest = depth;
        }

        if (server->t_prio < prio && server->t_state == THREAD_BLOCKED) {
            server->t_prio = prio;
            chain_raised++;
        }

        if (server->t_wait != THREAD_WAIT_SEND &&
          server->t_wait != THREAD_WAIT_REPLY) {
            return SYS_OK;
        }
        if (server->t_blocked_on == NULL) {
            return SYS_OK;
        }

        ch = server->t_blocked_on;
    }

    return SYS_ERR_DEADLOCK;
}

/*
 * The connections minted on this channel, and collecting the ones whose
 * peers have gone, per M18 stage four.
 *
 * The list is the server's claim on them, which is why it is on the
 * channel and not somewhere a server would have to hold a handle for:
 * a connection here is minted by whoever holds the receive right, and
 * in this system that is usually a parent handing one to a child at
 * spawn rather than the server itself. The channel is the one thing the
 * server does hold.
 */
void
channel_conn_add(struct channel *ch, struct connection *cn)
{
    intr_state_t s = md_intr_disable();

    cn->cn_next = ch->c_conns;
    ch->c_conns = cn;

    md_intr_restore(s);

    /*
     * Outside the region above, because the count is kept under the
     * lock the other reference counts are kept under rather than
     * under the one that guards this list.
     */
    channel_hold(ch);
}

void
channel_conn_remove(struct channel *ch, struct connection *cn)
{
    intr_state_t s = md_intr_disable();
    struct connection **link;

    for (link = &ch->c_conns; *link != NULL; link = &(*link)->cn_next) {
        if (*link == cn) {
            *link = cn->cn_next;
            break;
        }
    }
    cn->cn_next = NULL;

    md_intr_restore(s);

    channel_drop(ch);
}

/*
 * A connection's peers have all gone. The server is nudged with the
 * pulse of section 13.1, which is a hint and nothing more: it may be
 * coalesced with another, and it may be lost, and neither costs
 * anything because what says which connection it was is the list above.
 *
 * Only for a channel that collects, because a pulse to a server that
 * has no way to act on it is a slot spent on nothing.
 */
void
channel_conn_gone(struct channel *ch, struct connection *cn)
{
    intr_state_t s;
    uint32_t i;

    (void)cn;

    if (ch == NULL || !ch->c_reaping) {
        return;
    }

    /*
     * One at a time, and never two. This pulse says there is something
     * to collect and says nothing about what, so a second one queued
     * behind the first would tell the server nothing it does not
     * already know and would spend a slot doing it.
     *
     * That matters because the slot is a reserved one. A system pulse
     * arriving at a full queue panics, per section 13.1, and a client
     * may fill the six user slots whenever it likes, so a channel that
     * enqueued one of these per departing client would halt the
     * machine on the third departure that nobody had collected. The
     * queue is a hint and one hint is the whole of it.
     */
    s = md_intr_disable();
    for (i = 0; i < ch->c_pulse_count; i++) {
        uint32_t k = (ch->c_pulse_head + i) % SYS_PULSE_SLOTS;

        if (ch->c_pulses[k].code == SYS_PULSE_PEER_GONE) {
            md_intr_restore(s);
            return;
        }
    }
    md_intr_restore(s);

    channel_pulse(ch, SYS_PULSE_PEER_GONE, 0);
}

bool
channel_reaping(const struct channel *ch)
{
    return ch != NULL && ch->c_reaping && ch->c_owner != NULL;
}

/*
 * Collect one, per M18 stage four. The cookie goes back because it is
 * the server's own name for the connection and the only one it has:
 * the kernel never looked at it, and this is the one moment it hands it
 * back.
 *
 * One at a time, and the caller asks again. That is `waitpid` with
 * WNOHANG and it is the same loop for the same reason: what is
 * authoritative is the state, and the caller drains it rather than
 * counting how many times it was told.
 */
sys_err_t
channel_reap(struct channel *ch, bool enable, uint64_t *out)
{
    intr_state_t s = md_intr_disable();
    struct connection *cn;
    uint64_t cookie;

    if (enable) {
        ch->c_reaping = true;
    }

    for (cn = ch->c_conns; cn != NULL; cn = cn->cn_next) {
        if (!cn->cn_gone) {
            continue;
        }
        cookie = cn->cn_cookie;
        cn->cn_gone = false;
        channel_peers_collected++;
        md_intr_restore(s);

        /*
         * Collected, so what was keeping it is gone. Freed here only
         * if nothing else names it: the server may hold a handle of
         * its own, and then the last close frees it as it always did.
         * Outside the walk either way, because destroying it unlinks
         * it and the walk is what that would be changing.
         */
        if (connection_unheld(cn)) {
            connection_destroy(cn);
        }

        /*
         * And the answer is written last. It goes to memory the caller
         * named, which is the caller's to have unmapped or shared, and
         * writing it above would have been a write to whatever that is
         * with interrupts off and the channel half walked.
         */
        *out = cookie;

        return SYS_OK;
    }

    md_intr_restore(s);

    return SYS_ERR_NO_ENT;
}

/*
 * Something took a reference to this channel, or let one go, per M20
 * stage two.
 *
 * Under the lock the other reference counts are kept under, because
 * these are called from the same two places: a handle appearing in a
 * table and a handle leaving one. A connection is the other caller
 * and it arrives here through channel_conn_add and its pair.
 *
 * Nothing is freed here. Stage two exists to find out whether the
 * count can be made to agree with a scan before anything depends on
 * the answer, and a free written before that is a free written on
 * trust.
 */
void
channel_hold(struct channel *ch)
{
    intr_state_t s;

    if (ch == NULL) {
        return;
    }

    s = objref_enter();
    ch->c_holds++;
    objref_leave(s);
}

/*
 * Nothing names this channel and nobody is standing in it, so the
 * slot goes back. Per M20 stage three, and the first thing in this
 * system's life to return one.
 *
 * Three things say no, and each is a thread that is going to come
 * back to this channel: one queued to send on it, one blocked
 * receiving on it, and one inside a request on it. Freeing under any
 * of them hands the slot to somebody else while a thread is still
 * holding a pointer into it.
 *
 * A pulse in the queue is not one of them, and stage four is where
 * that was decided rather than assumed. A pulse is collected by
 * receiving on the channel, receiving needs a handle, and there are
 * no handles: nothing that exists can ever collect it. Keeping the
 * slot for it would be keeping a slot for a message with no possible
 * reader, so the pulses go with the channel and how many is counted.
 *
 * A channel that reaches zero holds while a thread is inside it is
 * not freed here. It is freed when that thread leaves, which is what
 * the calls to channel_maybe_free at the ends of the operations that
 * let a thread out are for.
 */

static void
channel_free_if_idle(struct channel *ch)
{
    intr_state_t s;
    intr_state_t ks = 0;
    bool took = false;
    bool under_lock;
    bool idle;

    /*
     * Under the kernel's exclusion, and taken here only if this core
     * does not already have it.
     *
     * Every call that arrives from a vector holds it, and a kernel
     * thread closing a handle does not: the boot closes handles
     * directly, and without this the test and the free would run on
     * one core while the other was inside a rendezvous putting a
     * thread on one of the lists being tested. Taking a lock this
     * core already holds is refused rather than deadlocked, so the
     * question has to be asked.
     */
    under_lock = klock_held();
    if (!under_lock) {
        ks = klock_enter();
        took = true;
    }

    s = md_intr_disable();
    idle = ch->c_senders == NULL && ch->c_receivers == NULL &&
      ch->c_serving == NULL;
    if (idle) {
        uint32_t i;

        /*
         * Counted by kind, because the two mean different things.
         * A user pulse nobody can collect is a message to a server
         * that no longer exists. A system pulse is section 13.1's
         * reserved kind, and the only one that reaches here is a
         * death notice for a client of a channel whose server has
         * already gone, which is a notice with nobody to act on it.
         * M18 moved the fact a notice carries onto the connection
         * for exactly this reason, so losing one costs latency and
         * not a record.
         */
        for (i = 0; i < ch->c_pulse_count; i++) {
            uint32_t k = (ch->c_pulse_head + i) % SYS_PULSE_SLOTS;

            if (ch->c_pulses[k].code < SYS_PULSE_USER_BASE) {
                channel_notices_discarded++;
            }
        }
        channel_pulses_discarded += ch->c_pulse_count;
    }
    md_intr_restore(s);

    if (!idle) {
        if (took) {
            klock_exit(ks);
        }
        return;
    }


    /*
     * The name goes first so that a pointer somebody kept says "?"
     * rather than the name of a channel that is not there any more.
     * The generation the pool advances is what makes a handle safe;
     * nothing makes a raw pointer safe, and the boot holds a good
     * many of those.
     */
    ch->c_name = NULL;
    ch->c_owner = NULL;
    channel_freed++;
    if (under_lock) {
        channel_freed_in_kernel++;
    }

    if (!pool_free(channel_pool(), ch->c_id)) {
        panic("channel: a slot refused to go back");
    }

    if (took) {
        klock_exit(ks);
    }
}

/*
 * A thread has left this channel. If nothing names it any more, the
 * thread that just left may have been the last thing keeping it.
 *
 * Called at the end of the operations that let a thread out, where
 * the caller is finished with the channel. Cheap when there is
 * nothing to do, which is every call but a handful in a boot: one
 * read of a counter under the lock the counter is kept under.
 */
uint32_t
channel_holds(const struct channel *ch)
{
    intr_state_t s = objref_enter();
    uint32_t n = ch->c_holds;

    objref_leave(s);

    return n;
}

void
channel_maybe_free(struct channel *ch)
{
    intr_state_t s;
    bool none;

    if (ch == NULL) {
        return;
    }

    s = objref_enter();
    none = ch->c_holds == 0;
    objref_leave(s);

    if (none) {
        channel_free_if_idle(ch);
    }
}

void
channel_drop(struct channel *ch)
{
    intr_state_t s;
    bool last;

    if (ch == NULL) {
        return;
    }

    s = objref_enter();
    if (ch->c_holds == 0) {
        objref_leave(s);
        panic("channel %s: let go of more often than held",
          ch->c_name != NULL ? ch->c_name : "?");
    }
    last = --ch->c_holds == 0;
    objref_leave(s);

    if (last) {
        channel_free_if_idle(ch);
    }
}

/*
 * How long one of a channel's thread lists is, refusing to walk
 * further than the machine has threads.
 */
static uint32_t
list_length(const struct channel *ch, const struct thread *head,
  const char *which, uint32_t limit)
{
    const struct thread *t;
    uint32_t n = 0;

    for (t = head; t != NULL; t = t->t_snext) {
        if (++n > limit) {
            panic("channel %s: its %s list passes %u threads and does "
              "not end", ch->c_name != NULL ? ch->c_name : "?", which,
              (unsigned int)limit);
        }
    }

    return n;
}

/*
 * Mark the slot a channel occupies, for the scan below.
 *
 * The bound is checked rather than assumed. Every pointer handed here
 * comes out of a structure this walks, and a structure being changed
 * under the walk could hand back something that is not a channel; an
 * index off the end of the array would be a write past the end of the
 * caller's stack, which is a much worse answer than a wrong count.
 */
static void
mark_channel(uint32_t *held, const struct channel *ch)
{
    uint32_t idx;

    if (ch == NULL) {
        return;
    }
    idx = sys_id_index(ch->c_id);
    if (idx >= SYS_MAX_CHANNELS) {
        panic("channels: a holder named slot %u of %u",
          (unsigned int)idx, (unsigned int)SYS_MAX_CHANNELS);
    }
    held[idx]++;
}

/*
 * How many channels in use nothing can reach, per M20 stage one.
 *
 * A scan rather than a count, because a count is the thing this
 * milestone has not got yet. Everything that can name a channel is
 * walked and what it names is marked, and what is left in use and
 * unmarked is a slot that will be occupied until the machine stops.
 *
 * Four kinds of holder, which is the list stage two has to turn into a
 * number the kernel maintains:
 *
 *   - A handle in some process's table. This is the authority of
 *     section 7 and the only one a program has.
 *   - A connection minted on it, per section 12.1. A pager is one of
 *     these, so a channel a process faults to is held this way and not
 *     only by whoever holds the handle.
 *   - A thread inside a request on it, which is t_serve_ch and is how
 *     section 12.3 gets from a queued sender to whoever is serving it.
 *   - A thread queued on it, receiving on it, or serving from it. The
 *     channel holds those lists rather than the other way round, but a
 *     channel with anything on one of them is a channel somebody is
 *     inside, and freeing it would take the floor out from under a
 *     thread that is going to come back to it.
 *
 * What no scan can see is the kernel's own pointers. The boot holds
 * channels in file-scope variables in init_main.c, and those are
 * exactly the leak this milestone is about rather than an exception to
 * it: a check made a channel, closed every handle to it, and nothing
 * in the system will ever put the slot back. They are counted as
 * unreachable because that is what they are.
 */
uint32_t channel_scans;
uint32_t channel_created;
uint32_t channel_freed;
uint32_t channel_freed_in_kernel;
uint32_t channel_pulses_discarded;
uint32_t channel_notices_discarded;
uint64_t channel_scan_first_ns;

void
channel_scan(struct channel_scan *out)
{
    const struct pool *chans = channel_pool();
    const struct pool *procs = proc_pool();
    const struct pool *threads = thread_pool();
    const struct pool *conns = connection_pool();
    uint32_t held[SYS_MAX_CHANNELS];
    uint32_t serving[SYS_MAX_CHANNELS];
    intr_state_t is;
    intr_state_t ks = 0;
    bool took = false;
    uint64_t t0, t1;
    uint32_t i;
    uint32_t left = 0;

    out->cs_stuck_name = NULL;
    uint32_t tables = 0;
    uint32_t stale = 0;
    uint32_t stuck = 0;

    for (i = 0; i < SYS_MAX_CHANNELS; i++) {
        held[i] = 0;
        serving[i] = 0;
    }

    /*
     * Under the kernel lock and with interrupts off, because the four
     * things this walks are the four things a running program changes.
     *
     * The boot thread never entered the kernel and so does not hold
     * the lock, which is what makes taking it here both necessary and
     * possible. Ten processes are still running when this is called,
     * and a handle table being edited on the other core while this
     * walks it would give a count that no change caused, which is the
     * one kind of number this project will not put in a compared file.
     * Interrupts as well as the lock, because at Tier 0 the lock is
     * nothing and one core is what makes interrupts off exclusion.
     *
     * Reading used and high-water in here too, so that all three
     * numbers on the line come from one reading of the pool rather
     * than from three.
     *
     * Timed from outside the region rather than inside it, so that
     * what is reported is how long interrupts were actually off. Time
     * spent spinning for the lock is part of that and would not be
     * counted by a clock read after the lock was taken.
     */
    t0 = time_now();
    is = md_intr_disable();
    if (!klock_held()) {
        ks = klock_enter();
        took = true;
    }

    for (i = 0; i < procs->count; i++) {
        const struct proc *p;
        uint32_t j;
        uint32_t found = 0;

        if (!procs->slots[i].inuse) {
            continue;
        }
        p = pool_lookup(procs, sys_id_make(i, procs->slots[i].gen));
        if (p == NULL) {
            continue;
        }
        /*
         * Every slot, and then the table's own count checked against
         * what was found. Stopping early once the count is accounted
         * for is the obvious saving and it is not taken: it costs
         * nothing measurable here, and it turns a table whose count
         * has drifted from its slots into a scan that quietly stops
         * short. A wrong count in a compared file is the failure this
         * milestone is least able to afford.
         */
        for (j = 0; j < SYS_MAX_HANDLES_PER_PROC; j++) {
            const struct handle_slot *h = &p->p_handles.ht_slots[j];

            if (!h->h_inuse) {
                continue;
            }
            found++;
            if (h->h_type != SYS_OBJ_CHANNEL) {
                continue;
            }
            mark_channel(held, h->h_object);
        }

        if (found != p->p_handles.ht_used) {
            panic("channels: a table says %u handles and holds %u",
              (unsigned int)p->p_handles.ht_used, (unsigned int)found);
        }
        tables++;

    }

    for (i = 0; i < conns->count; i++) {
        const struct connection *cn;

        if (!conns->slots[i].inuse) {
            continue;
        }
        cn = pool_lookup(conns, sys_id_make(i, conns->slots[i].gen));
        if (cn != NULL) {
            mark_channel(held, cn->cn_channel);
        }
    }

    /*
     * The threads inside a request, counted from the thread's side.
     *
     * Not added to held: serving_begin sets t_serve_ch and links the
     * thread onto c_serving in one place, so this is the same fact as
     * the channel's own list rather than a second holder. It is
     * counted anyway so that the two sides can be required to agree,
     * which is the only check there is that the pair stays a pair.
     */
    for (i = 0; i < threads->count; i++) {
        const struct thread *t;

        if (!threads->slots[i].inuse) {
            continue;
        }
        t = pool_lookup(threads, sys_id_make(i, threads->slots[i].gen));
        if (t != NULL) {
            mark_channel(serving, t->t_serve_ch);
        }
    }

    /*
     * And the count the kernel keeps against the count just made.
     *
     * This is the whole of the stage. Everything above walks what can
     * name a channel and arrives at a number per slot; c_holds is what
     * the kernel believes without walking anything. A holder nobody
     * wrote down shows up here as a slot the walk found and the kernel
     * did not, and one let go of twice shows up the other way.
     */
    for (i = 0; i < chans->count; i++) {
        const struct channel *ch;
        const struct thread *t;
        uint32_t inside = 0;

        if (!chans->slots[i].inuse) {
            if (held[i] != 0 || serving[i] != 0) {
                panic("channels: slot %u is free and %u things name it",
                  (unsigned int)i,
                  (unsigned int)(held[i] + serving[i]));
            }
            continue;
        }
        ch = pool_lookup(chans, sys_id_make(i, chans->slots[i].gen));
        if (ch == NULL) {
            continue;
        }

        if (ch->c_holds != held[i]) {
            panic("channel %s: the kernel says %u hold it and %u do",
              ch->c_name != NULL ? ch->c_name : "?",
              (unsigned int)ch->c_holds, (unsigned int)held[i]);
        }

        /*
         * The serving list against the threads that say they are on
         * it, and the two do not have to be equal.
         *
         * Every thread whose t_serve_ch names this channel is on this
         * list, because serving_begin does both in one place. The
         * other direction does not hold, and the reason is the
         * limitation section 19.3 states and nothing enforces: a
         * server thread that takes a second request while it still
         * owes a reply on the first is not refused, and it goes onto
         * a second channel's list without leaving the first. Its
         * t_serve_ch names the second, so the first list has an entry
         * that says it is somewhere else, and when the thread finally
         * ends its request it unlinks from the second list only.
         *
         * So what is asserted is the direction that holds. The other
         * way round is counted and reported, because it is a real
         * thing happening in a real session rather than a
         * possibility: the console driver does it when a line is
         * typed at it, and this walk is the first thing in this
         * kernel that has ever noticed.
         *
         * It costs nothing to stage three, because a list with
         * anything on it keeps the slot either way. A stale entry
         * makes a channel unfreeable rather than freeable, which is
         * the safe half of the mistake.
         */
        for (t = ch->c_serving; t != NULL; t = t->t_serve_next) {
            uint32_t ti = sys_id_index(t->t_id);

            if (ti >= threads->count || !threads->slots[ti].inuse) {
                panic("channel %s: its serving list names thread slot "
                  "%u, which is free",
                  ch->c_name != NULL ? ch->c_name : "?",
                  (unsigned int)ti);
            }
            if (++inside > threads->count) {
                panic("channel %s: its serving list passes %u threads "
                  "and does not end",
                  ch->c_name != NULL ? ch->c_name : "?",
                  (unsigned int)threads->count);
            }
        }
        if (serving[i] > inside) {
            panic("channel %s: %u threads say they serve it and %u "
              "are on its list",
              ch->c_name != NULL ? ch->c_name : "?",
              (unsigned int)serving[i], (unsigned int)inside);
        }
        stale += inside - serving[i];

        /*
         * Bounded, because a walk of a list is a walk of whatever the
         * list turns out to be. No channel can hold more threads than
         * the machine has, so a walk that passes that many has found a
         * loop, and a diagnostic that hangs the machine is worse than
         * one that says what it found.
         */
        inside += list_length(ch, ch->c_senders, "senders",
          threads->count);
        inside += list_length(ch, ch->c_receivers, "receivers",
          threads->count);

        /*
         * Nothing names it, nobody is standing in it and no pulse is
         * waiting, and yet here it is. Since stage three that cannot
         * happen at the moment the last holder lets go, so a slot in
         * this state became free afterwards, when the last thread
         * left a list or the last pulse was collected, and nothing
         * looked again. Counted and asserted to be none, because the
         * day it is not is the day something has to look again.
         *
         * The lists are counted rather than being given a field of
         * their own, per the note on c_holds: a thread on one of them
         * is a fact the channel already holds.
         */
        if (held[i] == 0 && inside == 0 && ch->c_pulse_count == 0) {
            left++;
        }

        /*
         * And the ones the free could not take: nothing names them,
         * and something is standing in them or a pulse is waiting.
         * Named, because one number says how many and the name says
         * which, and stage four needs to know which.
         */
        if (held[i] == 0 && (inside != 0 || ch->c_pulse_count != 0)) {
            stuck++;
            if (out->cs_stuck_name == NULL) {
                out->cs_stuck_name = ch->c_name;
            }
        }
    }

    out->cs_used = chans->used;
    out->cs_made = channel_created;
    out->cs_given_back = channel_freed;
    out->cs_at_once = chans->highwater;
    out->cs_of = chans->count;
    out->cs_unreachable = left;
    channel_scans++;

    out->cs_tables = tables;
    out->cs_stale_serving = stale;
    out->cs_stuck = stuck;
    out->cs_conns = conns->count;
    out->cs_threads = threads->count;

    if (took) {
        klock_exit(ks);
    }
    md_intr_restore(is);
    t1 = time_now();

    out->cs_ns = (t1 - t0) * 1000000000ULL / time_hz();

    /*
     * The first walk kept, because it is ten times the others and the
     * difference is the emulator rather than the kernel. A walk this
     * code has not made before is a walk QEMU has to translate before
     * it can run, and the translation is paid once: the first is about
     * 700 microseconds and every later one is about 70. Anything read
     * from the first figure is a statement about a just-in-time
     * compiler.
     */
    if (channel_scan_first_ns == 0) {
        channel_scan_first_ns = out->cs_ns;
    }
}
