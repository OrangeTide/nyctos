/* vm.h : address spaces, mapping flags and the port VM interface.
 *
 * SPDX-License-Identifier: 0BSD OR Apache-2.0
 * Copyright (C) 2026 Jon Mayo <jon@rm-f.net>
 *
 * Declares the VM_* mapping flags, struct addrspace, the vm_cost accounting
 * structure, and the md_* functions that a port implements for page tables and
 * TLB maintenance. Also declares the vm_* functions that create, destroy and
 * count address spaces. Included by the cohort, process, object, thread, pager,
 * grant, channel, CPU and syscall sources and by the aarch64 and riscv64 pmap
 * code.
 *
 */

#ifndef _SYS_VM_H_
#define _SYS_VM_H_

#include <sys/config.h>
#include <sys/op.h>
#include <sys/page.h>

#include <stdbool.h>
#include <stdint.h>

/*
 * What a mapping permits. These are the permissions, in the form the
 * hardware wants them: what may be done, and by whom.
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
 * looks like a driver bug.
 */
#define VM_DEVICE       (1u << 4)

/*
 * What a loaded program's own memory is mapped as.
 *
 * proc_prot() answers what a page of a program may be used for, from
 * where it sits: text read and executed, everything else read and
 * written, the startup record read only. What is left for this name is
 * the pages that are not a program's own, which the loader does not place
 * and nothing knows the shape of.
 */
#define VM_PROGRAM      (VM_READ | VM_WRITE | VM_EXEC | VM_USER)

struct addrspace {
    uintptr_t   as_root;        /* physical address of the port's root table */

    /*
     * What the hardware files this space's translations under, or zero
     * for a space the hardware cannot tell apart from another.
     *
     * Zero is not an identifier, it is the absence of one, and a space
     * that has it is activated by throwing every translation away. Two
     * spaces may hold it at once and remain correct for that reason.
     * See vm_as_create(), which hands them out, and md_asid_bits(),
     * which says how many the port has.
     */
    uint32_t    as_asid;
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

    /*
     * And how many times one identifier's worth of translations was
     * thrown away, which happens when a space stops existing and its
     * name goes back to be given to another. Counted always rather than
     * inside the buffer path's bracket, because it is not the buffer
     * path: recycling a name is a separate event.
     */
    uint64_t    vc_asid_inval;

    /* What was asked of them. */
    uint64_t    vc_pages;
    uint64_t    vc_halves;

    /*
     * And how many times a thread was dispatched into a different
     * address space, which is not the buffer path and is counted
     * beside it for scale. A number for the buffer path with nothing
     * to compare it against would be a number with no size.
     */
    uint64_t    vc_switch;

    /*
     * How many of those had to throw the whole translation cache away,
     * which is every one of them on a port with no identifiers and
     * none of them on a port with some. The two counted separately
     * because the difference between them is what is being measured.
     */
    uint64_t    vc_switch_flush;
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

/* Called by a port when it throws one identifier's translations away. */
void vm_count_asid_inval(void);

/*
 * Called by the rendezvous for each half of a move, saying how many
 * pages that half carried. The two counts have different denominators
 * on purpose: an entry is written per page, and the translation cache
 * is thrown away once per half whatever the page count.
 */
void vm_charge_pages(uint32_t pages);

/*
 * Called by the dispatcher when it enters a different address space,
 * saying whether the port had to throw the translation cache away to
 * get there.
 */
void vm_count_switch(bool flushed);

/* What has been spent so far. */
void vm_cost_read(struct vm_cost *out);

/*
 * The same, for one core alone. Every event these count happens on a
 * core, so the sum above is a sum and this is what it is a sum of.
 */
void vm_cost_read_core(struct vm_cost *out, uint32_t core);

/* Build an empty address space, or return SYS_ERR_NO_MEMORY. */
sys_err_t md_as_create(struct addrspace *as);

/*
 * Give one back, freeing the tables under it and nothing they point at.
 * The leaves name a process's slot, which is returned before this, and
 * the kernel's own mappings, which every space carries and none owns.
 * Idempotent: a space with no root is already gone.
 */
void md_as_destroy(struct addrspace *as);

/*
 * How many frames its tables are, without taking it apart. The same
 * walk md_as_destroy makes, counting instead of freeing.
 */
uint32_t md_as_frames(const struct addrspace *as);

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
 *
 * Returns whether it threw the translation cache away, which it has to
 * do for a space with no identifier and does not for a space with one.
 * The caller counts it; the port is what knows.
 */
bool md_as_activate(const struct addrspace *as);

/*
 * How many bits of identifier this port has, or zero if it has none it
 * can use. Implementation defined on both, so it is asked of the
 * hardware rather than assumed: a width written down in a header is a
 * width that is wrong on the first board nobody tried.
 *
 * Answered once and remembered. It must be asked before the first
 * address space is built, because what it does is write a value and
 * read back what stuck.
 */
uint32_t md_asid_bits(void);

/*
 * What identifier the hardware is running under right now, read back
 * out of the register that holds it.
 *
 * Asked rather than remembered, because what a check wants to know is
 * what was written and not what the writer intended. A port that builds
 * the register value and drops the identifier out of it answers zero
 * here while every other count in the system stays right.
 */
uint32_t md_as_current_asid(void);

/*
 * Whether the leaf that covers this address says its translation
 * belongs to one space rather than to the machine.
 *
 * The two ports spell it in opposite directions, which is why this is
 * asked of the port and not read from a bit somebody named. riscv64
 * tags every entry unless it is marked global; aarch64 tags none of
 * them unless it is marked not global. What both answer is the same
 * question, and the answer says whether an identifier means anything
 * for this address.
 *
 * SYS_ERR_INVALID_ARG if nothing covers the address. Any leaf answers,
 * at whatever level the port put it, because the kernel's own mappings
 * are blocks and a check that could only ask about pages could not ask
 * about them.
 */
sys_err_t md_leaf_tagged(const struct addrspace *as, uintptr_t va,
  bool *out);

/* Turn translation on for the first time, in this space. */
void md_mmu_enable(const struct addrspace *as);

/* Everything, for a change too broad to describe. */
void md_tlb_flush_all(void);

/*
 * One address, for a change that names the addresses it made.
 *
 * Across every address space rather than the current one, which is what
 * makes a single call correct for a page moving between two of them: it
 * removes one page of one address from every space at once, and for a
 * page being handed over that is exactly the two entries that changed.
 * Coarser than it looks, and still far narrower than throwing the whole
 * cache away.
 *
 * Identifiers do not cost anything here yet. Once riscv64 tags its
 * translations an entry does carry the space it came from, but the
 * instruction this uses names no identifier and every space is still
 * covered by one call, so nothing here had to change. Whether one call
 * across every space is dearer than two that name the two spaces that
 * matter is a measurement and not an argument. The signature would grow
 * the space if the answer said so, so it is left out today rather than
 * carried as a parameter nothing reads.
 */
void md_tlb_invalidate(uintptr_t va);

/*
 * One identifier, for a space that is about to stop existing.
 *
 * Everything the hardware learned while running under that name goes,
 * and nothing filed under any other name does. What this is for is
 * handing the name to somebody else: a space that inherits an
 * identifier inherits the translations filed under it, and this is what
 * makes sure there are none.
 *
 * Local to the core that calls it on riscv64, which is what that port's
 * fences are, so it says so through vm_note_local_maintenance() and the
 * other core throws everything away at its next switch. Inner-shareable
 * on aarch64, which reaches the other core itself.
 */
void md_tlb_invalidate_asid(uint32_t asid);

/*
 * One address in one space, which is the narrow form of the two above.
 *
 * The broad call removes an address from every space at once, which for
 * a page being handed between two of them is the two entries that
 * changed and nothing else. This removes one of them. Whether two of
 * these are cheaper than one of those is a measurement.
 */
void md_tlb_invalidate_in(uint32_t asid, uintptr_t va);

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

/*
 * And take it down. The kernel's mappings in it are not the kernel's
 * memory, so what this returns is the tables and the root.
 */
void vm_as_destroy(struct addrspace *as);

/*
 * Said by a port whose TLB maintenance reaches only the core that
 * performed it, every time it performs some. riscv64's fences are
 * core-local and say this; aarch64's are inner-shareable and reach the
 * other core themselves, so that port says nothing.
 *
 * Entries that another core has invalidated must not survive on this core
 * past its next switch. A core flushes at its first switch after
 * maintenance happened anywhere, and not otherwise.
 *
 * Not a substitute for cross-core maintenance, which this kernel still
 * does not have. It is the difference between an entry that survives a
 * switch and one that survives the run.
 */
void vm_note_local_maintenance(void);

/*
 * How much of it the other cores have done, which is what the calling
 * core compares against. Its own does not count: a core that threw an
 * entry away is not the core still holding it.
 */
uint64_t vm_maint_epoch(void);

/*
 * And how much every core has done between them, which is zero on a
 * port whose fences reach the other core by themselves. What reads it
 * is the boot report, to know whether a switch that threw everything
 * away was something to expect.
 */
uint64_t vm_maint_noted(void);

/*
 * And how much of it this core has done, which is what says whether
 * this port's fences reach anybody else. A port whose maintenance is
 * inner-shareable notes none and moves this not at all.
 */
uint64_t vm_maint_here(void);

/*
 * How many identifiers have been given out, how many of those were
 * somebody else's first, how many spaces got none at all, and how many
 * names are out on loan now.
 */
void vm_asid_counts(uint32_t *given, uint32_t *reused, uint32_t *none,
  uint32_t *live);

/*
 * Whether entering this space means writing the root register, given
 * what a core is already running and how much maintenance it has not
 * heard about.
 *
 * The dispatcher's rule, in one place because it is the rule that keeps
 * a core from running one space through another space's translations,
 * and because a rule inside an `if` cannot be asked a question. See
 * kern_thread.c, which asks it on every switch, and asid_checks(),
 * which asks it about the cases a boot does not reach.
 */
bool vm_as_needs_entry(const struct addrspace *as, uintptr_t root,
  uint32_t asid, uint64_t seen, uint64_t now);

/* The window loaded programs occupy, which no space maps for the kernel. */
void vm_user_window(uintptr_t *base, size_t *len);

/* The kernel's space, for a caller that needs to map something into it. */
struct addrspace *vm_kernel(void);

#endif /* _SYS_VM_H_ */
