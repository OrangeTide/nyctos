/*
 * SPDX-License-Identifier: 0BSD OR Apache-2.0
 *
 * PUBLIC DOMAIN (CC0-1.0)
 */

/*
 * Grants. See sys/sys/grant.h for what one is and section 19.2 for why a
 * driver has one.
 */

#include <sys/grant.h>
#include <sys/object.h>
#include <sys/page.h>
#include <sys/proc.h>
#include <sys/systm.h>
#include <sys/vm.h>

struct grant *
grant_create(const char *name, uintptr_t base, size_t len, bool device)
{
    struct grant *g;

    if (len == 0 || (base & PAGE_MASK) != 0 || (len & PAGE_MASK) != 0) {
        panic("grant %s: 0x%lx+0x%lx is not whole pages", name,
          (unsigned long)base, (unsigned long)len);
    }

    g = pool_alloc(grant_pool(), NULL);
    if (g == NULL) {
        return NULL;
    }

    g->g_id = pool_id_of(grant_pool(), g);
    g->g_name = name;
    g->g_base = base;
    g->g_len = len;
    g->g_device = device;

    return g;
}

sys_err_t
grant_map(struct grant *g, struct proc *p, uint32_t rights,
  uintptr_t *out_base, uintptr_t *out_len)
{
#if SYS_TIER == 2
    uint32_t flags = VM_USER;
    sys_err_t err;

    if (rights & SYS_RIGHT_READ) {
        flags |= VM_READ;
    }
    if (rights & SYS_RIGHT_WRITE) {
        flags |= VM_READ | VM_WRITE;
    }
    if (rights & SYS_RIGHT_EXEC) {
        flags |= VM_EXEC;
    }
    if (g->g_device) {
        flags |= VM_DEVICE;
    }

    /*
     * A mapping that permits nothing is not a mapping. One port would
     * make it a page nobody may touch and the other would make it a
     * page table, so a holder whose handle carries no access to the
     * memory is told it has none rather than given something.
     */
    if ((flags & (VM_READ | VM_WRITE | VM_EXEC)) == 0) {
        return SYS_ERR_PERM_DENIED;
    }

    err = md_map(proc_as(p), g->g_base, g->g_base, g->g_len, flags);
    if (err != SYS_OK) {
        return err;
    }
    md_tlb_flush_all();
#else
    (void)p;
    (void)rights;
#endif

    *out_base = g->g_base;
    *out_len = g->g_len;

    return SYS_OK;
}

/*
 * Take a grant back out of a process, per section 7: the handle is the
 * authority and a mapping that outlived one would be authority that
 * outlived it too.
 *
 * The region reverts to what every other address space has for it rather
 * than being removed. A grant covers memory the kernel maps as well, and
 * clearing the entry would take the console away from the kernel in that
 * process's space, which the kernel finds out about the next time it
 * prints. What goes is the bit that lets user mode in.
 */
void
grant_unmap(struct grant *g, struct proc *p)
{
#if SYS_TIER == 2
    uint32_t flags = VM_READ | VM_WRITE;

    if (p == NULL) {
        return;
    }
    if (g->g_device) {
        flags |= VM_DEVICE;
    }

    if (md_map(proc_as(p), g->g_base, g->g_base, g->g_len, flags) !=
      SYS_OK) {
        panic("grant %s: cannot be taken back from %s", g->g_name,
          p->p_name);
    }
    md_tlb_flush_all();
#else
    (void)g;
    (void)p;
#endif
}
