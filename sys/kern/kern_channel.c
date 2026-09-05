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
#include <sys/object.h>
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

    ch->c_id = pool_id_of(channel_pool(), ch);
    ch->c_name = name;
    ch->c_owner = owner;
    ch->c_senders = NULL;
    ch->c_receivers = NULL;

    return ch;
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

void
channel_unwait_thread(void *obj, struct thread *t)
{
    struct channel *ch = obj;

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
        if (md_map(to, va, pa, PAGE_SIZE, VM_PROGRAM) != SYS_OK) {
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
         * was taken away from the sender at that same address, and
         * neither port tags a translation with the space it came
         * from, so removing that address removes both entries. A
         * port with address space identifiers would need two calls
         * here rather than one, which is a cost this counts as one
         * because it is not paying it.
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

sys_err_t
channel_send(struct channel *ch, uint64_t cookie, const struct sys_msg *req,
  struct sys_msg *reply)
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
         * Nobody is at a receive point, so this thread queues. Section
         * 12.3 has the queue being priority-ordered serve it, and that
         * is only half: whoever is inside a request on this channel
         * may be blocked further down, and everything below is running
         * at the priority of the client it is serving rather than of
         * this one.
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
    }
    trace_wait(self, self->t_wait);

    self->t_blocked_on = ch;
    self->t_unwait = channel_unwait_thread;

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

    err = self->t_waitres;

    /*
     * Nothing may still be lent out. A reply gives the buffer back
     * before it releases this thread, and a reply is the only thing that
     * releases it: a sender carries no deadline and cancellation does
     * not exist yet. When it does, this is where it will be found, and
     * the alternative to finding it is a program running again with a
     * hole where its memory was.
     */
    if (self->t_lend_len != 0) {
        panic("ipc: %s ran again with its buffer still lent out",
          self->t_name);
    }


    /*
     * The reply is in this thread's staging message, put there by
     * whoever replied, and this thread is running again: its own
     * memory is the memory that is mapped, so now it can be copied
     * where the caller asked for it.
     *
     * Whether a reply happened is the question, not whether it
     * succeeded. A handle that failed to move is a reply with an
     * error in it and a payload the server still wrote, and the
     * first version of this dropped the payload on the floor for
     * every error the reply path can report.
     */
    if (self->t_replied) {
        msg_copy(reply, &self->t_stage);
    }

    self->t_wait = THREAD_WAIT_NONE;
    self->t_msg = NULL;
    self->t_reply = NULL;

    md_intr_restore(s);

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
static void
pulse_lost_flag(struct channel *ch, struct sys_msg *out)
{
    if (ch->c_pulse_lost) {
        out->flags |= SYS_MSG_PULSE_LOST;
        ch->c_pulse_lost = false;
    }
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
    pulse_lost_flag(ch, out);

    return true;
}

sys_err_t
channel_recv(struct channel *ch, struct sys_msg *out,
  sys_thread_id_t *out_peer, uint64_t *out_cookie)
{
    struct thread *self = thread_current();
    struct thread *client;
    intr_state_t s;

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

            pulse_lost_flag(ch, out);
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

        if (self->t_waitres != SYS_OK) {
            *out_peer = self->t_client;
            *out_cookie = self->t_cookie;
            md_intr_restore(s);

            return self->t_waitres;
        }

        /*
         * A sender filled this thread's staging message while this
         * thread was blocked. Copy it out now that this thread's memory
         * is what is mapped.
         */
        if (self->t_client != 0) {
            msg_copy(out, &self->t_stage);
            pulse_lost_flag(ch, out);
            *out_peer = self->t_client;
            *out_cookie = self->t_cookie;
            md_intr_restore(s);

            return SYS_OK;
        }

        /* Woken for a pulse. Round again to take it, or to wait anew. */
    }
}

/*
 * Reply, and stop running at the client's priority.
 *
 * A reply to a client that has died is refused rather than delivered,
 * because the id carries a generation and the slot it names is no longer
 * the thread that sent the request.
 */
sys_err_t
channel_reply(sys_thread_id_t peer, const struct sys_msg *reply)
{
    struct thread *self = thread_current();
    struct thread *client;
    intr_state_t s;
    sys_err_t err;

    s = md_intr_disable();

    client = thread_by_id(peer);
    if (client == NULL || client->t_reply == NULL) {
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

void
channel_thread_cancelled(struct thread *t)
{
    serving_end(t);

    channel_thread_release(t, true);
}

/*
 * Enqueue a pulse, per section 13.1. Never blocks, and never fails for
 * want of room: what a full queue costs is an older pulse, not this one.
 *
 * The rule is one line and the reason is the whole of why pulses are
 * specified at all. User pulses may fill the queue up to the reserved
 * slots and no further; when they are at that limit the oldest user
 * pulse is dropped to make room and the channel remembers that it
 * happened. A system pulse may use the reserved slots, so a process
 * that floods a channel with pulses of its own cannot push a death
 * notice out, and a dropped death notice leaks a dead client's
 * resources for good.
 *
 * The oldest goes rather than the newest, which is worth stating
 * because it is a choice. A notification is about something that has
 * happened, and the older one is the one whose news has had longest to
 * be overtaken.
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
        /*
         * Drop the oldest user pulse. It need not be at the head: a
         * system pulse can be in front of it, and taking that instead
         * would be user pulses displacing a death notice by a longer
         * route.
         */
        uint32_t i;

        for (i = 0; i < ch->c_pulse_count; i++) {
            uint32_t k = (ch->c_pulse_head + i) % SYS_PULSE_SLOTS;
            uint32_t j;

            if (ch->c_pulses[k].code < SYS_PULSE_USER_BASE) {
                continue;
            }
            for (j = i; j + 1 < ch->c_pulse_count; j++) {
                uint32_t a = (ch->c_pulse_head + j) % SYS_PULSE_SLOTS;
                uint32_t b = (ch->c_pulse_head + j + 1) % SYS_PULSE_SLOTS;

                ch->c_pulses[a] = ch->c_pulses[b];
            }
            ch->c_pulse_count--;
            ch->c_pulse_users--;
            ch->c_pulse_lost = true;
            break;
        }
    }

    if (ch->c_pulse_count == SYS_PULSE_SLOTS) {
        /*
         * Only reachable for a system pulse against a queue of system
         * pulses, which means more deaths are outstanding than the
         * reservation was sized for. Losing one of those is the thing
         * the reservation exists to prevent, so it stops rather than
         * being silently absorbed.
         */
        panic("channel %s: the reserved pulse slots are full", ch->c_name);
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
 * Release one client blocked on a channel whose server is gone.
 * Interrupts are already off.
 */
static void channel_thread_release(struct thread *t, bool tell);

static void
release_dead(struct thread *t)
{
    /*
     * And the link that says it is serving something, because it is
     * not any more. A dead server left on a channel's serving list is
     * a thread the chain walk of section 12.3 would follow into a slot
     * that has been given to something else.
     */
    serving_end(t);

    /*
     * Its buffer first, for the reason cancellation has: the pages are
     * in the server and gone from here, the reply that would have given
     * them back is not coming, and a thread released into its own
     * memory with a hole in it is worse than the wait it was in.
     *
     * And no notice. The server is the thing that has gone, so there
     * is nobody to tell: leaving a death notice on a channel whose
     * owner has terminated is a pulse nothing will ever collect, and
     * enough of them would fill the slots reserved for the notices
     * that do have a reader.
     */
    channel_thread_release(t, false);

    t->t_waitres = SYS_ERR_OWNER_DEAD;
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
            release_dead(t);
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
            release_dead(t);
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
