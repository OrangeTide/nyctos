/*
 * SPDX-License-Identifier: 0BSD OR Apache-2.0
 *
 * PUBLIC DOMAIN (CC0-1.0)
 */

/*
 * A page fault, sent as a message by the thread that took it.
 *
 * Section 15.3 is the whole of this file's reason. The kernel does not
 * call a pager, deliver an upcall, or queue a request: it makes the
 * faulting thread an ordinary blocked sender to an ordinary channel, and
 * every mechanism the rest of the system already has applies without
 * being told about paging.
 *
 * Priority inheritance is the one worth naming. A high-priority thread's
 * fault is serviced at high priority because the thread donates to its
 * pager exactly as any client donates to any server, and nothing here
 * arranges that. It falls out of sending rather than calling.
 */

#include <sys/channel.h>
#include <sys/connection.h>
#include <sys/grant.h>
#include <sys/handle.h>
#include <sys/intr.h>
#include <sys/page.h>
#include <sys/proc.h>
#include <sys/systm.h>
#include <sys/thread.h>
#include <sys/vm.h>

#if SYS_TIER == 2

/*
 * Take the page the reply carried and put it where the fault was.
 *
 * The grant says which memory and the fault says where, which is the one
 * place in this system that a grant is not mapped at its own address:
 * section 15.3 has the pager reply with a grant covering the faulting
 * address, and the faulting address is the client's rather than the
 * page's.
 *
 * The handle is closed afterwards. The process was handed authority over
 * a page and the mapping is what that authority is for; leaving the
 * handle would leave a program a way to revoke its own text.
 */
static sys_err_t
pager_install(struct proc *p, uintptr_t va, sys_handle_t h)
{
    struct grant *g;
    uint32_t flags = VM_READ | VM_USER;
    uint32_t rights;
    void *obj;
    sys_err_t err;

    if (handle_lookup(&p->p_handles, h, SYS_OBJ_GRANT, SYS_RIGHT_READ,
      &obj) != SYS_OK) {
        return SYS_ERR_INVALID_ARG;
    }
    if (handle_rights(&p->p_handles, h, &rights) != SYS_OK) {
        return SYS_ERR_STALE;
    }
    g = obj;
    if (g->g_len < PAGE_SIZE || g->g_device) {
        return SYS_ERR_INVALID_ARG;
    }

    /*
     * A page supplied to a fault is mapped by the kernel, at an address
     * the pager did not choose, so a holder that could map it itself
     * would be holding a handle that means something different from
     * what was done with it: closing it would take back the region at
     * the grant's own address and leave the mapping at the faulting one.
     * Refused rather than reconciled.
     */
    if ((rights & SYS_RIGHT_MAP) != 0) {
        return SYS_ERR_INVALID_ARG;
    }

    /*
     * What the mapping permits is what the grant carries, as everywhere
     * else, with one subtraction: section 15.1 has a pager supply
     * read-only pages and nothing else, so a grant carrying write does
     * not become a writable mapping on this path. Execute is the
     * difference between a page of text and a page of anything else,
     * and assuming it made every page a pager supplied executable.
     */
    if ((rights & SYS_RIGHT_EXEC) != 0) {
        flags |= VM_EXEC;
    }

    err = md_map(proc_as(p), va, g->g_base, PAGE_SIZE, flags);
    if (err != SYS_OK) {
        return err;
    }
    md_tlb_flush_all();

    (void)handle_close(&p->p_handles, h);

    return SYS_OK;
}

/*
 * Ask this thread's pager for the page it faulted on. Returns true when
 * the page is there and the instruction can be tried again.
 */
bool
pager_fault(uintptr_t addr, uintptr_t pc, uint32_t access)
{
    struct thread *self = thread_current();
    struct proc *p = self != NULL ? self->t_proc : NULL;
    sys_fault_msg_t *fm;
    struct sys_msg req;
    struct sys_msg reply;
    uintptr_t page;

    if (p == NULL || p->p_pager == NULL) {
        return false;
    }

    /*
     * Section 15.4 rule 3. A process on a paging path that faults is a
     * configuration mistake, and the alternative to stopping here is a
     * pager waiting for a page only it could have supplied.
     */
    if (p->p_locked) {
        panic("%s is memory-locked and faulted at 0x%lx, pc 0x%lx",
          p->p_name, (unsigned long)addr, (unsigned long)pc);
    }

    /*
     * Section 15.1: a pager supplies read-only pages, so a fault that
     * wanted to write is not one a pager can answer. Refused here rather
     * than after the reply, because asking costs the pager a frame and a
     * grant and the answer was never going to be usable.
     */
    if (access == SYS_RIGHT_WRITE) {
        return false;
    }

    page = addr & ~(uintptr_t)PAGE_MASK;

    memset(&req, 0, sizeof(req));
    req.handle = SYS_ID_INVALID;
    req.inline_len = sizeof(*fm);
    fm = (sys_fault_msg_t *)req.inline_data;
    fm->addr = page;
    fm->pc = pc;
    fm->access = access;

    p->p_faults++;

    if (channel_send(p->p_pager->cn_channel, p->p_pager->cn_cookie,
      &req, &reply) != SYS_OK) {
        return false;
    }
    if (reply.handle == SYS_ID_INVALID) {
        return false;
    }

    return pager_install(p, page, reply.handle) == SYS_OK;
}

#endif /* SYS_TIER == 2 */
