/*
 * SPDX-License-Identifier: 0BSD OR Apache-2.0
 * Copyright (C) 2026 Jon Mayo <jon@rm-f.net>
 */

/*
 * Address spaces, and the surface a port provides under them.
 *
 * Section 27 asks a Tier 2 port for two things: page tables and TLB
 * maintenance. This is what that means in practice. Nothing above the port
 * knows what a page table looks like, how many levels it has, or which
 * bits mean what; what it knows is that a range of virtual addresses can
 * be made to name a range of physical ones with a set of permissions, and
 * that a space can be made the current one.
 *
 * An address space is one word here, which is the physical address of
 * whatever the port uses as a root. A port that needs more than that keeps
 * it in the same structure, and nothing above it looks inside.
 *
 * Tier 0 has none of this. The whole file is behind SYS_TIER, because the
 * alternative is a set of functions that exist and refuse, which is a way
 * of making a caller ask a question that has no meaning at that tier.
 */

#ifndef _SYS_VM_H_
#define _SYS_VM_H_

#include <sys/config.h>
#include <sys/op.h>
#include <sys/page.h>

#include <stdint.h>

/*
 * What a mapping permits. These are the permissions section 6.1 talks
 * about, in the form the hardware wants them: what may be done, and by
 * whom.
 */
#define VM_READ         (1u << 0)
#define VM_WRITE        (1u << 1)
#define VM_EXEC         (1u << 2)
#define VM_USER         (1u << 3)   /* reachable from user mode */

/*
 * Device memory rather than ordinary memory. It is a separate flag rather
 * than an attribute of the address because getting it wrong does not
 * fault: a device mapped as ordinary memory is a device whose registers
 * are cached, reordered and speculated, which produces corruption that
 * looks like a driver bug. Section 27 names it for that reason.
 */
#define VM_DEVICE       (1u << 4)

struct addrspace {
    uintptr_t   as_root;        /* physical address of the port's root table */
};

/* Build an empty address space, or return SYS_ERR_NO_MEMORY. */
sys_err_t md_as_create(struct addrspace *as);

/*
 * Map len bytes of virtual addresses onto physical ones. Both addresses
 * and the length must be page-aligned; the port chooses whatever page or
 * block size the range allows, which is invisible above here except in
 * how many frames it consumes.
 *
 * It performs no TLB maintenance, because a mapping is usually one of
 * several and the caller knows when it has finished. A caller that maps
 * into a space something is already running in flushes afterwards.
 */
sys_err_t md_map(struct addrspace *as, uintptr_t va, uintptr_t pa,
  size_t len, uint32_t flags);

/*
 * What a virtual address names in this space, or SYS_ERR_INVALID_ARG if
 * it names nothing. A caller that has to hand one process's page to
 * another has to ask this: the page tables are the only record of which
 * frame is behind a program's address.
 */
sys_err_t md_lookup(const struct addrspace *as, uintptr_t va,
  uintptr_t *out_pa);

/*
 * Take a range of pages out of a space. Like md_map it performs no TLB
 * maintenance, and unlike md_map that matters: an entry that is gone
 * from the table and still in the TLB is a mapping that is still there.
 */
void md_unmap(struct addrspace *as, uintptr_t va, size_t len);

/*
 * Make this space the current one, with whatever maintenance that needs.
 * This is a switch and not an enable: it does not touch the registers that
 * describe the table format, because changing those while translation is
 * live is a different operation with different rules.
 */
void md_as_activate(const struct addrspace *as);

/* Turn translation on for the first time, in this space. */
void md_mmu_enable(const struct addrspace *as);

/* Everything, for a change too broad to describe. */
void md_tlb_flush_all(void);

/*
 * Map the registers this port's kernel reaches directly: the console it
 * panics through, and the interrupt controller. A driver's device memory
 * is not here, because a driver is a process and gets a grant.
 */
sys_err_t md_map_devices(struct addrspace *as);

/*
 * Build the kernel's own address space and enable translation. After this
 * returns the kernel is running through page tables, which on both ports
 * means the instruction after the one that enabled them was fetched
 * through them.
 */
void vm_kernel_init(void);

/* Build an address space with the kernel in it, ready for a program. */
sys_err_t vm_as_create(struct addrspace *as);

/* The window loaded programs occupy, which no space maps for the kernel. */
void vm_user_window(uintptr_t *base, size_t *len);

/* The kernel's space, for a caller that needs to map something into it. */
struct addrspace *vm_kernel(void);

#endif /* _SYS_VM_H_ */
