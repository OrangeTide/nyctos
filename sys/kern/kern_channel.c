/*
 * SPDX-License-Identifier: 0BSD OR Apache-2.0
 * Copyright (C) 2026 Jon Mayo <jon@rm-f.net>
 */

/*
 * Send, receive, reply. See sys/sys/channel.h for the shape and section 12
 * for the reasoning.
 */

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

static void
msg_copy(struct sys_msg *dst, const struct sys_msg *src)
{
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
    }

    md_tlb_flush_all();

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

#if SYS_TIER == 2
    {
        struct addrspace *to = proc_as(server->t_proc);
        uintptr_t first = m->buf & ~(uintptr_t)PAGE_MASK;
        uintptr_t last = (m->buf + m->buf_len - 1) & ~(uintptr_t)PAGE_MASK;
        uintptr_t sbase, rbase;
        size_t slen, rlen;
        uintptr_t va;

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
         * Nothing to do when the two run in the same memory, which is a
         * server and a client in one process.
         */
        if (proc_as(client->t_proc) == to) {
            return SYS_OK;
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
         */
        md_tlb_flush_all();
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
    if (req->buf_len > 0 &&
      !proc_owns(self->t_proc, req->buf, req->buf_len)) {
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
        server->t_blocked_on = NULL;
        server->t_waitres = SYS_OK;

        server->t_wait = THREAD_WAIT_NONE;
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
        sender_insert(ch, self);
        self->t_wait = THREAD_WAIT_SEND;
    }
    trace_wait(self, self->t_wait);

    self->t_blocked_on = ch;
    self->t_unwait = channel_unwait_thread;
    sched_block();

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

sys_err_t
channel_recv(struct channel *ch, struct sys_msg *out,
  sys_thread_id_t *out_peer, uint64_t *out_cookie)
{
    struct thread *self = thread_current();
    struct thread *client;
    intr_state_t s;

    s = md_intr_disable();

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
             * reach it, so there is no message to serve. The client
             * is told, rather than left blocked on a rendezvous that
             * did not happen, and this thread goes on to wait like
             * one that found nothing queued.
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

        donate(self, client->t_prio);

        /*
         * The client moves from SEND-blocked to REPLY-blocked without
         * running. It was queued and is now being served.
         */
        client->t_wait = THREAD_WAIT_REPLY;
        trace_wait(client, THREAD_WAIT_REPLY);

        self->t_wait = THREAD_WAIT_NONE;

        *out_peer = self->t_client;
        *out_cookie = self->t_cookie;
        md_intr_restore(s);

        return SYS_OK;
    }

    /*
     * Nothing records where the message will go: a sender writes
     * this thread's staging message, and this thread copies it out
     * when it runs again and its own memory is mapped.
     */
    self->t_msg = &self->t_stage;
    self->t_snext = ch->c_receivers;
    ch->c_receivers = self;

    self->t_wait = THREAD_WAIT_RECV;
    trace_wait(self, THREAD_WAIT_RECV);

    self->t_blocked_on = ch;
    self->t_unwait = channel_unwait_thread;
    sched_block();

    /*
     * A sender filled this thread's staging message while this
     * thread was blocked. Copy it out now that this thread's memory
     * is what is mapped.
     */
    if (self->t_waitres == SYS_OK) {
        msg_copy(out, &self->t_stage);
    }

    *out_peer = self->t_client;
    *out_cookie = self->t_cookie;

    md_intr_restore(s);

    return self->t_waitres;
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
    self->t_client = 0;

    sched_ready(client);

    md_intr_restore(s);

    return err;
}
