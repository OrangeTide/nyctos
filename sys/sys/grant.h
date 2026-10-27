/* grant.h : grants, handles to physical memory regions.
 *
 * SPDX-License-Identifier: 0BSD OR Apache-2.0
 * Copyright (C) 2026 Jon Mayo <jon@rm-f.net>
 *
 * Declares struct grant, a physical region with a base address, a length,
 * and a device flag. Also declares the functions that create a grant, map
 * it for a holder, and unmap it. grant_frames() reports the frames that
 * grants hold. Included by the kernel init, grant, object, operation, and
 * pager code.
 *
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
     * way that reads as a driver bug.
     */
    bool            g_device;
};

/*
 * Make a grant over a region. Returns NULL when the pool is empty.
 * This is called by whatever assembles the system, which is
 * the kernel at boot, and a system that cannot describe its own devices
 * is one that should say so rather than continue.
 */
struct grant *grant_create(const char *name, uintptr_t base, size_t len,
  bool device);

/*
 * The frames grants are holding. A grant describes memory it does not
 * own, so this is the count of what nothing will give back. Device
 * grants are not counted: their memory is a chip.
 */
uint32_t grant_frames(void);

/*
 * Put the region where the holder can reach it, and say where that is.
 * The rights are the holder's, from the handle it presented, so a grant
 * carrying only SYS_RIGHT_READ becomes a read-only mapping.
 *
 * At Tier 0 there is nothing to map and this reports the address the
 * region already has. That is the weaker tier exactly: the
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

/*
 * Take it back out again, when the handle that carried it goes. The
 * region reverts to the mapping every other address space has for it,
 * which is the kernel's own without user access.
 */
void grant_unmap(struct grant *g, struct proc *p);

#endif /* _SYS_GRANT_H_ */
