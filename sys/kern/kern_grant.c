/* kern_grant.c : grants of whole-page memory regions to processes.
 *
 * SPDX-License-Identifier: 0BSD OR Apache-2.0
 * Copyright (C) 2026 Jon Mayo <jon@rm-f.net>
 *
 * grant_create records a page-aligned region and whether it is device memory,
 * and panics on a region that is not whole pages. grant_map and grant_unmap
 * add and remove the region in a process with the requested rights.
 * grant_frames totals the frames in non-device grants.
 *
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
 * Take a grant back out of a process. The handle is the authority, and a
 * mapping that outlived one would be authority that outlived it too.
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

/*
 * The frames grants are holding.
 *
 * A grant over ordinary memory holds it. grant_create is handed a
 * physical address and does not own it, so nothing in a grant's
 * teardown gives anything back: the caller found the memory and the
 * grant only describes it. Whoever allocated it is the owner, and in
 * this system that is a check or a pager, neither of which frees.
 *
 * A device grant is skipped, because its memory is a chip. Counting it
 * would report frames the allocator never had.
 */
uint32_t
grant_frames(void)
{
    struct pool *p = grant_pool();
    uint32_t total = 0;
    uint32_t i;

    for (i = 0; i < p->count; i++) {
        const struct grant *g = pool_at(p, i);

        if (g == NULL || g->g_device) {
            continue;
        }
        total += (uint32_t)(g->g_len / PAGE_SIZE);
    }

    return total;
}
