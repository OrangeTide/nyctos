/*
 * SPDX-License-Identifier: 0BSD OR Apache-2.0
 *
 * PUBLIC DOMAIN (CC0-1.0)
 */

/*
 * Grants: authority over a region of memory, held as a handle.
 *
 * Section 6.1 has two forms in which memory crosses a boundary, and the
 * difference between them is lifetime rather than semantics. A message
 * buffer is memory lent for one call. A grant is memory delegated for as
 * long as the holder keeps the handle: a device's registers, a page a
 * pager supplied, a target for a device to write into.
 *
 * What a grant covers is a physical region. It is not a promise about a
 * virtual address, because a virtual address means something only inside
 * one address space and a grant can be held by more than one process.
 * The address a holder sees is decided when it maps the grant, and this
 * kernel gives it the physical address: every other mapping here is the
 * identity, the regions a grant covers are device registers outside
 * every program's window, and a holder that had to be told a different
 * number would be a holder that had to ask.
 *
 * Nothing in the system can mint one. There is no operation that turns a
 * physical address into authority, because that operation would be
 * ambient authority over the machine: whoever could call it could reach
 * any device. What assembles the system makes them, per section 19.2,
 * and hands them to the processes that need them. Here the kernel is
 * what assembles the system.
 */

#ifndef _SYS_GRANT_H_
#define _SYS_GRANT_H_

#include <sys/op.h>
#include <sys/pool.h>

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

struct proc;

struct grant {
    sys_id_t        g_id;
    const char     *g_name;

    /* The region, in physical addresses, page-aligned at both ends. */
    uintptr_t       g_base;
    size_t          g_len;

    /*
     * Device registers rather than ordinary memory. It is a property of
     * the region and not of the holder, because what makes a mapping
     * wrong here is caching and reordering rather than permission, and
     * getting it wrong does not fault: it corrupts, at a distance, in a
     * way that reads as a driver bug. Section 27 names it for that.
     */
    bool            g_device;
};

/*
 * Make a grant over a region. Returns NULL when the pool is empty, per
 * section 8: this is called by whatever assembles the system, which is
 * the kernel at boot, and a system that cannot describe its own devices
 * is one that should say so rather than continue.
 */
struct grant *grant_create(const char *name, uintptr_t base, size_t len,
  bool device);

/*
 * Put the region where the holder can reach it, and say where that is.
 * The rights are the holder's, from the handle it presented, so a grant
 * carrying only SYS_RIGHT_READ becomes a read-only mapping.
 *
 * At Tier 0 there is nothing to map and this reports the address the
 * region already has. That is section 6.1's weaker tier exactly: the
 * grant is what makes the permission explicit rather than what creates
 * it, and a holder that never asked could have reached the registers
 * anyway.
 *
 * Mapping twice is not an error. The second call describes the same
 * region at the same address, because the mapping is the identity and
 * writing an entry that is already there changes nothing.
 */
sys_err_t grant_map(struct grant *g, struct proc *p, uint32_t rights,
  uintptr_t *out_base, uintptr_t *out_len);

#endif /* _SYS_GRANT_H_ */
