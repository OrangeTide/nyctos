/*
 * SPDX-License-Identifier: 0BSD OR Apache-2.0
 * Copyright (C) 2026 Jon Mayo <jon@rm-f.net>
 */

/*
 * Page tables, AArch64.
 *
 * Four kilobyte granule, thirty-nine bits of virtual address, which is
 * three levels: one gigabyte at level one, two megabytes at level two,
 * four kilobytes at level three. Thirty-nine bits is enough for every
 * address either port uses and costs a level less than forty-eight would.
 *
 * Only TTBR0 is used. A kernel in the high half through TTBR1 is the
 * arrangement most systems reach for, and it would mean relinking the
 * kernel to run at an address it is not loaded at, which buys nothing
 * this milestone spends. What it would buy is a kernel that stays mapped
 * across an address space switch without being in every table, and that
 * is worth revisiting when there are many address spaces rather than one.
 */

#include <sys/intr.h>
#include <sys/page.h>
#include <sys/systm.h>
#include <sys/vm.h>

#if SYS_TIER == 2

#define ENTRIES         512
#define LEVEL_SHIFT(l)  (39 - (l) * 9)      /* l = 1, 2, 3 */
#define LEVEL_SIZE(l)   ((uintptr_t)1 << LEVEL_SHIFT(l))
#define INDEX(va, l)    (((va) >> LEVEL_SHIFT(l)) & (ENTRIES - 1))

#define DESC_TABLE      0x3u
#define DESC_BLOCK      0x1u
#define DESC_PAGE       0x3u

#define ATTR_IDX(n)     ((uint64_t)(n) << 2)
#define ATTR_NS         (1ULL << 5)
#define ATTR_AP_RW_EL1  (0ULL << 6)
#define ATTR_AP_RW_ALL  (1ULL << 6)
#define ATTR_AP_RO_EL1  (2ULL << 6)
#define ATTR_AP_RO_ALL  (3ULL << 6)
#define ATTR_SH_INNER   (3ULL << 8)
#define ATTR_AF         (1ULL << 10)

/*
 * Not global: file this translation under whatever identifier was
 * current when it was made, and consult it only under that one.
 *
 * Set on every leaf this port writes, per M19 stage three. The other
 * port had to be given an identifier and its entries were tagged
 * already; here the entries are what had to change, and an identifier
 * written into the root register was a number nothing read. That is the
 * whole of the difference between the two ports, and it runs in
 * opposite directions: there, marking an entry global would be an
 * optimisation, and here it is what every entry already was.
 *
 * None of them is left global, for M19 stage two's reason. The kernel's
 * identity map looks like the one thing that is the same in every
 * space, and it is not: a grant rewrites the kernel's own entry for the
 * address it covers, with user access, in the holder's space and no
 * other.
 */
#define ATTR_NG         (1ULL << 11)
#define ATTR_PXN        (1ULL << 53)
#define ATTR_UXN        (1ULL << 54)

/* MAIR attribute slots: ordinary memory and device registers. */
#define MAIR_NORMAL     0
#define MAIR_DEVICE     1
#define MAIR_VALUE      (0xffULL << (8 * MAIR_NORMAL))

/*
 * TCR: 39-bit VA on TTBR0, 4KB granule, inner-shareable, write-back
 * cacheable walks, and TTBR1 walks disabled because nothing is mapped
 * there. IPS is 32 bits, which covers every address on this board.
 */
#define TCR_VALUE       ((25ULL << 0)  | /* T0SZ: 64 - 39 */                \
                         (0ULL << 14)  | /* TG0: 4KB */                     \
                         (3ULL << 12)  | /* SH0: inner shareable */         \
                         (1ULL << 10)  | /* ORGN0: write-back */            \
                         (1ULL << 8)   | /* IRGN0: write-back */            \
                         (25ULL << 16) | /* T1SZ, unused but must be sane */\
                         (1ULL << 23)  | /* EPD1: no TTBR1 walks */         \
                         (0ULL << 32))   /* IPS: 32 bits */

/*
 * Where the identifier sits in the root register, and the bit in TCR
 * that says how much of it is read. AS selects sixteen bits; with it
 * clear only the low eight of the field mean anything.
 */
#define TTBR_ASID_SHIFT 48
#define TCR_AS          (1ULL << 36)

#define SCTLR_M         (1ULL << 0)
#define SCTLR_C         (1ULL << 2)
#define SCTLR_I         (1ULL << 12)

/*
 * The root register's value for a space: which tables, and which space.
 */
static uint64_t
as_ttbr0(const struct addrspace *as)
{
    return (uint64_t)as->as_root |
      ((uint64_t)as->as_asid << TTBR_ASID_SHIFT);
}

static uint64_t
leaf_attrs(uint32_t flags, bool block)
{
    uint64_t e = block ? DESC_BLOCK : DESC_PAGE;

    e |= ATTR_AF | ATTR_NG;
    if (flags & VM_DEVICE) {
        e |= ATTR_IDX(MAIR_DEVICE);
    } else {
        e |= ATTR_IDX(MAIR_NORMAL) | ATTR_SH_INNER;
    }

    if (flags & VM_USER) {
        e |= (flags & VM_WRITE) ? ATTR_AP_RW_ALL : ATTR_AP_RO_ALL;
    } else {
        e |= (flags & VM_WRITE) ? ATTR_AP_RW_EL1 : ATTR_AP_RO_EL1;
    }

    /*
     * Execute permission is two bits, one per privilege level, and both
     * are refusals. A mapping that is not executable by a level has that
     * level's never-execute bit set, and device memory is never executable
     * by anyone.
     */
    if ((flags & VM_EXEC) == 0 || (flags & VM_DEVICE) != 0) {
        e |= ATTR_PXN | ATTR_UXN;
    } else if ((flags & VM_USER) == 0) {
        e |= ATTR_UXN;
    }

    return e;
}

static uint64_t *
table_at(uintptr_t pa)
{
    return (uint64_t *)pa;
}

/*
 * The table one level down from this entry, made if it is not there.
 *
 * A block that covers the range is split into the table it stands for:
 * the same memory, the same attributes, described one page at a time.
 * Splitting is what lets anything refine a mapping the identity map made
 * out of blocks, which is every grant over ordinary memory and every
 * page a pager will supply. Without it the kernel could only ever change
 * a mapping it had made in pages to begin with, which means deciding in
 * advance which memory might one day be interesting.
 *
 * Returns NULL when there is no frame for the table, which is the only
 * failure this walk has.
 */
static uint64_t *
descend(uint64_t *table, uint32_t index, unsigned int level)
{
    uintptr_t pa;
    uint64_t *next;

    if ((table[index] & 0x3u) == DESC_TABLE) {
        return table_at(table[index] & 0x0000fffffffff000ULL);
    }

    pa = page_alloc();
    if (pa == 0) {
        return NULL;
    }
    next = table_at(pa);
    vm_charge_table();

    if ((table[index] & 0x3u) == DESC_BLOCK) {
        uint64_t block = table[index];
        uintptr_t out = (uintptr_t)(block & 0x0000fffffffff000ULL);
        uint64_t attrs = block & ~0x0000fffffffff000ULL;
        size_t step = LEVEL_SIZE(level + 1);
        uint32_t i;

        /*
         * A block descriptor and a page descriptor differ in the low two
         * bits and in nothing else that matters here, so the attributes
         * carry across unchanged and only the output address moves.
         */
        attrs = (attrs & ~(uint64_t)0x3u) |
          (level + 1 == 3 ? DESC_PAGE : DESC_BLOCK);
        for (i = 0; i < ENTRIES; i++) {
            next[i] = (uint64_t)(out + (uintptr_t)i * step) | attrs;
        }
    }

    table[index] = (uint64_t)pa | DESC_TABLE;

    return next;
}

sys_err_t
md_as_create(struct addrspace *as)
{
    uintptr_t pa = page_alloc();

    if (pa == 0) {
        return SYS_ERR_NO_MEMORY;
    }
    as->as_root = pa;

    return SYS_OK;
}

/*
 * Give an address space's tables back, per M13 stage 3.
 *
 * What is freed is tables and only tables. Every leaf under this root
 * points at memory somebody else owns: a process's slot, which stage
 * two returned before this runs, or the kernel's own image and devices,
 * which map_kernel_into wrote into every space and which freeing would
 * take out from under the kernel itself.
 *
 * Telling one from the other needs the level here, because this format
 * gives a table and a page the same two bits: 0x3 at level one or two
 * is a table, and 0x3 at level three is a page. Reading the descriptor
 * without knowing where it sits would walk into a mapped page and free
 * whatever its contents happened to look like.
 *
 * The tables are private to this space even though what they describe
 * is not. map_kernel_into calls md_map, and md_map builds its own way
 * down, which is why an address space costs five frames here and not
 * one.
 */
static void
free_table(uintptr_t pa, unsigned int level)
{
    uint64_t *t = table_at(pa);
    uint32_t i;

    if (level < 3) {
        for (i = 0; i < ENTRIES; i++) {
            if ((t[i] & 0x3u) != DESC_TABLE) {
                continue;
            }
            free_table(t[i] & 0x0000fffffffff000ULL, level + 1);
        }
    }

    page_free(pa);
}

void
md_as_destroy(struct addrspace *as)
{
    if (as->as_root == 0) {
        return;
    }

    free_table(as->as_root, 1);

    /*
     * Cleared, so that a space destroyed twice is caught by proc_as
     * answering with the kernel's rather than by page_free panicking on
     * a frame it has already taken back.
     */
    as->as_root = 0;
}

/*
 * How many frames the tables under this root are, per M13 stage 6.
 *
 * The same walk md_as_destroy makes, counting where that one frees, and
 * level-aware for the same reason: this format gives a table and a page
 * the same two bits, and only the level tells them apart.
 */
static uint32_t
count_table(uintptr_t pa, unsigned int level)
{
    uint64_t *t = table_at(pa);
    uint32_t n = 1;
    uint32_t i;

    if (level < 3) {
        for (i = 0; i < ENTRIES; i++) {
            if ((t[i] & 0x3u) != DESC_TABLE) {
                continue;
            }
            n += count_table(t[i] & 0x0000fffffffff000ULL, level + 1);
        }
    }

    return n;
}

uint32_t
md_as_frames(const struct addrspace *as)
{
    if (as->as_root == 0) {
        return 0;
    }

    return count_table(as->as_root, 1);
}

sys_err_t
md_map(struct addrspace *as, uintptr_t va, uintptr_t pa, size_t len,
  uint32_t flags)
{
    uint64_t *l1 = table_at(as->as_root);

    if ((va & PAGE_MASK) != 0 || (pa & PAGE_MASK) != 0 ||
      (len & PAGE_MASK) != 0) {
        return SYS_ERR_INVALID_ARG;
    }

    while (len > 0) {
        uint64_t *l2 = descend(l1, (uint32_t)INDEX(va, 1), 1);
        size_t step;

        if (l2 == NULL) {
            return SYS_ERR_NO_MEMORY;
        }

        /*
         * A two megabyte block where the range allows one, which for the
         * identity map of all memory is every entry after the first few.
         * The alternative is five hundred and twelve times as many frames
         * spent on tables describing a mapping that never changes.
         */
        if (len >= LEVEL_SIZE(2) && (va & (LEVEL_SIZE(2) - 1)) == 0 &&
          (pa & (LEVEL_SIZE(2) - 1)) == 0) {
            l2[INDEX(va, 2)] = (uint64_t)pa | leaf_attrs(flags, true);
            vm_charge_leaf();
            step = LEVEL_SIZE(2);
        } else {
            uint64_t *l3 = descend(l2, (uint32_t)INDEX(va, 2), 2);

            if (l3 == NULL) {
                return SYS_ERR_NO_MEMORY;
            }
            l3[INDEX(va, 3)] = (uint64_t)pa | leaf_attrs(flags, false);
            vm_charge_leaf();
            step = PAGE_SIZE;
        }

        va += step;
        pa += step;
        len -= step;
    }

    return SYS_OK;
}

sys_err_t
md_map_devices(struct addrspace *as)
{
    sys_err_t err;

    /* The GIC: distributor at the base, CPU interface above it. */
    err = md_map(as, 0x08000000UL, 0x08000000UL, 0x00200000UL,
      VM_READ | VM_WRITE | VM_DEVICE);
    if (err != SYS_OK) {
        return err;
    }

    /*
     * The PL011, one page, which is what it is. It was two megabytes
     * because that was the size the identity map liked, and a block is
     * exactly what a grant cannot refine: section 19.2 has a driver
     * given this page with user access in its own space, and a mapping
     * the size of a block would have to be split to allow it.
     *
     * Not reachable from user mode. It was, for three stages, because
     * the driver held the address as a constant and would have faulted
     * on the first register it touched; it holds a grant now, and this
     * is the deviation those stages were carrying.
     */
    return md_map(as, 0x09000000UL, 0x09000000UL, 0x1000UL,
      VM_READ | VM_WRITE | VM_DEVICE);
}

/*
 * How wide this port's identifier field is: eight bits or sixteen, and
 * which one is the hardware's to say.
 *
 * Asked of ID_AA64MMFR0_EL1 rather than written and read back, which is
 * the other port's method because the other port has no register that
 * answers. Here one does, and its answer is the architecture's own.
 * Sixteen is taken when it is offered, because an identifier space that
 * runs out is a space that has to be recycled, and recycling is the
 * expensive part.
 *
 * Answered once and remembered, because md_mmu_enable has to put the
 * matching selection into TCR_EL1 and the second core has to make the
 * same choice as the first.
 */
uint32_t
md_asid_bits(void)
{
    static uint32_t bits;
    static bool asked;
    uint64_t mmfr0;

    if (asked) {
        return bits;
    }

    __asm__ volatile ("mrs %0, id_aa64mmfr0_el1" : "=r" (mmfr0));

    bits = (((mmfr0 >> 4) & 0xFULL) == 2) ? 16u : 8u;
    asked = true;

    return bits;
}

/*
 * The field is in the top sixteen bits of the root register, and this
 * reads it whether or not anything writes it. Today nothing does, so
 * this answers zero, which is the true answer and not a placeholder.
 */
sys_err_t
md_leaf_tagged(const struct addrspace *as, uintptr_t va, bool *out)
{
    uint64_t *table = table_at(as->as_root);
    unsigned int level;

    for (level = 1; level <= 3; level++) {
        uint64_t e = table[INDEX(va, level)];

        if ((e & 0x3u) == 0) {
            return SYS_ERR_INVALID_ARG;
        }
        if ((e & 0x3u) == DESC_BLOCK || level == 3) {
            *out = (e & ATTR_NG) != 0;
            return SYS_OK;
        }
        table = table_at(e & 0x0000fffffffff000ULL);
    }

    return SYS_ERR_INVALID_ARG;
}

uint32_t
md_as_current_asid(void)
{
    uint64_t ttbr0;

    __asm__ volatile ("mrs %0, ttbr0_el1" : "=r" (ttbr0));

    return (uint32_t)((ttbr0 >> TTBR_ASID_SHIFT) &
      ((1ULL << md_asid_bits()) - 1));
}

bool
md_as_activate(const struct addrspace *as)
{
    /*
     * A switch, not an enable. It writes the root register and
     * deliberately does not touch the registers that describe the
     * format: changing those while translation is live is a different
     * operation with different rules, and doing it on every context
     * switch was how this port ended up raising an undefined
     * instruction on a valid one.
     *
     * The identifier rides in the top of the same register as the root,
     * so one write says both which tables and which space. A space with
     * one keeps what the hardware learned about everywhere else; a
     * space with none is a space the hardware cannot tell from any
     * other with none, so entering it means throwing everything away.
     */
    __asm__ volatile ("msr ttbr0_el1, %0" :: "r" (as_ttbr0(as)));
    __asm__ volatile ("isb");

    if (as->as_asid == 0) {
        md_tlb_flush_all();
        return true;
    }

    return false;
}


void
md_mmu_enable(const struct addrspace *as)
{
    uint64_t sctlr;

    /*
     * The identifier width goes into TCR here and nowhere else, because
     * this is the one place that is allowed to write it: changing the
     * registers that describe the format while translation is live is a
     * different operation with different rules. Both cores come through
     * here and md_asid_bits() answers both of them the same.
     */
    __asm__ volatile ("msr mair_el1, %0" :: "r" (MAIR_VALUE));
    __asm__ volatile ("msr tcr_el1, %0" :: "r" ((uint64_t)TCR_VALUE |
      (md_asid_bits() == 16 ? TCR_AS : 0)));
    __asm__ volatile ("msr ttbr0_el1, %0" :: "r" (as_ttbr0(as)));
    __asm__ volatile ("isb");

    /*
     * The instruction cache may hold lines fetched with translation
     * off. They are the same instructions at the same addresses,
     * because the map is identity, so this is belt and braces; it is
     * also the kind of thing that is impossible to debug later.
     */
    __asm__ volatile ("ic iallu");
    __asm__ volatile ("dsb ish");
    __asm__ volatile ("isb");

    md_tlb_flush_all();

    /*
     * The instruction after the one that turns translation on is
     * fetched through the tables it just installed. Identity mapping
     * is what makes that survivable: the address the pipeline is
     * already working on means the same thing on both sides.
     */
    __asm__ volatile ("mrs %0, sctlr_el1" : "=r" (sctlr));
    sctlr |= SCTLR_M | SCTLR_C | SCTLR_I;
    __asm__ volatile ("msr sctlr_el1, %0" :: "r" (sctlr));
    __asm__ volatile ("isb");
}

void
md_tlb_flush_all(void)
{
    vm_charge_flush();

    __asm__ volatile ("dsb ishst");
    __asm__ volatile ("tlbi vmalle1is");
    __asm__ volatile ("dsb ish");
    __asm__ volatile ("isb");
}

/*
 * One page, in every address space identifier. This port assigns none,
 * so vaae1is is what its translations are actually filed under; the
 * operand is the address shifted down to a page number, which is the
 * encoding the instruction takes.
 */
void
md_tlb_invalidate(uintptr_t va)
{
    uint64_t page = (uint64_t)va >> 12;

    vm_charge_inval();

    __asm__ volatile ("dsb ishst");
    __asm__ volatile ("tlbi vaae1is, %0" :: "r" (page));
    __asm__ volatile ("dsb ish");
    __asm__ volatile ("isb");
}

void
md_tlb_invalidate_in(uint32_t asid, uintptr_t va)
{
    uint64_t arg = ((uint64_t)asid << TTBR_ASID_SHIFT) |
      ((uint64_t)va >> 12);

    vm_charge_inval();

    /*
     * The operand carries both, the identifier where the root register
     * keeps it and the page number below. Inner-shareable, so it
     * reaches the other core by itself. It reaches only entries whose
     * leaf said nG, which is every entry this port writes.
     */
    __asm__ volatile ("dsb ishst");
    __asm__ volatile ("tlbi vae1is, %0" :: "r" (arg));
    __asm__ volatile ("dsb ish");
    __asm__ volatile ("isb");
}

void
md_tlb_invalidate_asid(uint32_t asid)
{
    uint64_t arg = (uint64_t)asid << TTBR_ASID_SHIFT;

    vm_count_asid_inval();

    /*
     * Inner-shareable, so this reaches the other core by itself and
     * nothing has to be noted for it. The identifier goes in the same
     * bits of the operand that it occupies in the root register.
     *
     * It reaches only entries whose leaf said nG, which is every entry
     * this port writes and is what M19 stage three made true. Leave one
     * leaf global and this call stops covering it, and what is left
     * behind is a translation belonging to a space that no longer
     * exists, under a name somebody else is about to be given.
     */
    __asm__ volatile ("dsb ishst");
    __asm__ volatile ("tlbi aside1is, %0" :: "r" (arg));
    __asm__ volatile ("dsb ish");
    __asm__ volatile ("isb");
}


/*
 * What a virtual address names, for a caller handing a page to another
 * address space. Only a page-granule mapping answers: a block covers
 * more than the caller asked about, and the ranges this is used on are
 * program memory, which is mapped a page at a time.
 */
sys_err_t
md_lookup(const struct addrspace *as, uintptr_t va, uintptr_t *out_pa)
{
    uint64_t *l1 = table_at(as->as_root);
    uint64_t e;

    e = l1[INDEX(va, 1)];
    if ((e & 0x3u) != DESC_TABLE) {
        return SYS_ERR_INVALID_ARG;
    }
    e = table_at(e & 0x0000fffffffff000ULL)[INDEX(va, 2)];
    if ((e & 0x3u) != DESC_TABLE) {
        return SYS_ERR_INVALID_ARG;
    }
    e = table_at(e & 0x0000fffffffff000ULL)[INDEX(va, 3)];
    if ((e & 0x3u) != DESC_PAGE) {
        return SYS_ERR_INVALID_ARG;
    }

    *out_pa = (uintptr_t)(e & 0x0000fffffffff000ULL);

    return SYS_OK;
}

/*
 * Take a range of pages out of a space. The tables the pages hung from
 * stay: they cost a frame each, and a range that is unmapped here is
 * mapped again in the same place often enough that keeping them is the
 * cheaper of the two mistakes.
 */
void
md_unmap(struct addrspace *as, uintptr_t va, size_t len)
{
    uint64_t *l1 = table_at(as->as_root);

    while (len > 0) {
        uint64_t e = l1[INDEX(va, 1)];

        if ((e & 0x3u) == DESC_TABLE) {
            e = table_at(e & 0x0000fffffffff000ULL)[INDEX(va, 2)];
            if ((e & 0x3u) == DESC_TABLE) {
                table_at(e & 0x0000fffffffff000ULL)[INDEX(va, 3)] = 0;
                vm_charge_leaf();
            }
        }

        va += PAGE_SIZE;
        len -= PAGE_SIZE;
    }
}
#endif /* SYS_TIER == 2 */
