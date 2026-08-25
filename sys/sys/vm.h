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

/*
 * What a loaded program's own memory is mapped as. Its text, its data
 * and its stack are one slot and nothing above the loader knows where
 * one ends, so all of it is everything; teaching the archive to carry
 * the boundaries is what would let a program's text be read-only.
 *
 * It is a name rather than a constant repeated because the loader is no
 * longer the only thing that maps a program's page: a buffer that goes
 * out to a receiver and comes back has to come back as what it was.
 */
#define VM_PROGRAM      (VM_READ | VM_WRITE | VM_EXEC | VM_USER)

struct addrspace {
    uintptr_t   as_root;        /* physical address of the port's root table */
};

/*
 * What moving a buffer costs, counted where it is spent.
 *
 * Five numbers. Three are what the ports spent and two are what they
 * were asked for, and the rate is the one divided by the other.
 *
 * The three costs are three different costs. A leaf is a page-table entry written, which is a store. A
 * table is an intermediate level built, which is a store and a frame
 * that is never given back. A flush is the whole translation cache
 * thrown away, which is neither and is usually the expensive one.
 *
 * At Tier 0 all three stay zero, and that is the measurement rather
 * than a gap in it: there is one address space and a buffer that
 * arrives is already where it needs to be.
 */
struct vm_cost {
    /* What the ports wrote. */
    uint64_t    vc_leaf;
    uint64_t    vc_table;
    uint64_t    vc_flush;
    uint64_t    vc_inval;

    /* What was asked of them. */
    uint64_t    vc_pages;
    uint64_t    vc_halves;

    /*
     * And how many times a thread was dispatched into a different
     * address space, which is not the buffer path and is counted
     * beside it for scale. Neither port tags translations with the
     * space they came from, so every one of these throws the whole
     * cache away too. A number for the buffer path with nothing to
     * compare it against would be a number with no size.
     */
    uint64_t    vc_switch;
};

/*
 * Charge what follows to the buffer path, until the matching end.
 * Nests, because the move undoes itself by calling its other half.
 */
void vm_charge_begin(void);
void vm_charge_end(void);

/* Called by the ports as they spend it. */
void vm_charge_leaf(void);
void vm_charge_table(void);
void vm_charge_flush(void);
void vm_charge_inval(void);

/*
 * Called by the rendezvous for each half of a move, saying how many
 * pages that half carried. The two counts have different denominators
 * on purpose: an entry is written per page, and the translation cache
 * is thrown away once per half whatever the page count.
 */
void vm_charge_pages(uint32_t pages);

/* Called by the dispatcher when it enters a different address space. */
void vm_count_switch(void);

/* What has been spent so far. */
void vm_cost_read(struct vm_cost *out);

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
 * One address, for a change that names the addresses it made.
 *
 * Across every address space rather than the current one, because
 * neither port tags its translations with which space they came from.
 * That makes this coarser than it looks and still far narrower than
 * throwing the whole cache away: it removes one page of one address
 * from every space at once, which for a page being moved between two
 * spaces is exactly the two entries that changed.
 *
 * That one call covering every space is not a convenience, it is what
 * makes a single call correct for a page moving between two of them.
 * The day either port assigns address space identifiers, an entry
 * carries the space it came from, this stops covering both, and the
 * callers here have to name the space and call twice. Both would then
 * be cheaper than what this replaced, and neither would be this
 * function: the signature would grow the space, so it is left out
 * today rather than carried as a parameter nothing reads.
 */
void md_tlb_invalidate(uintptr_t va);

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
