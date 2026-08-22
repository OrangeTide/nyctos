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

#define SCTLR_M         (1ULL << 0)
#define SCTLR_C         (1ULL << 2)
#define SCTLR_I         (1ULL << 12)

static uint64_t
leaf_attrs(uint32_t flags, bool block)
{
    uint64_t e = block ? DESC_BLOCK : DESC_PAGE;

    e |= ATTR_AF;
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
 * Returns NULL when there is no frame for it, which is the only failure
 * this walk has.
 */
static uint64_t *
descend(uint64_t *table, uint32_t index)
{
    uintptr_t pa;

    if ((table[index] & 0x3u) == DESC_TABLE) {
        return table_at(table[index] & 0x0000fffffffff000ULL);
    }
    if (table[index] != 0) {
        panic("pmap: descending into a block at index %u", index);
    }

    pa = page_alloc();
    if (pa == 0) {
        return NULL;
    }
    table[index] = (uint64_t)pa | DESC_TABLE;

    return table_at(pa);
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
        uint64_t *l2 = descend(l1, (uint32_t)INDEX(va, 1));
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
            step = LEVEL_SIZE(2);
        } else {
            uint64_t *l3 = descend(l2, (uint32_t)INDEX(va, 2));

            if (l3 == NULL) {
                return SYS_ERR_NO_MEMORY;
            }
            l3[INDEX(va, 3)] = (uint64_t)pa | leaf_attrs(flags, false);
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
     * The PL011, which is a megabyte further up and was left out of
     * the first version of this. Nothing said so: the kernel enabled
     * translation and the next thing it did was print, which faulted,
     * and the fault printed through the same device. A machine that
     * stops with nothing on the console is what an unmapped console
     * looks like.
     *
     * User mode can reach it, which is a deviation with a stage
     * against it. The console driver holds its registers as an
     * address compiled into it, per section 19.2's grant that does
     * not exist yet, and an address space the driver did not ask for
     * does not contain its UART: it faults on the first register it
     * touches. Stage 7 hands it a grant and this flag goes.
     */
    return md_map(as, 0x09000000UL, 0x09000000UL, 0x00200000UL,
      VM_READ | VM_WRITE | VM_DEVICE | VM_USER);
}

void
md_as_activate(const struct addrspace *as)
{
    uint64_t sctlr;

    __asm__ volatile ("msr mair_el1, %0" :: "r" (MAIR_VALUE));
    __asm__ volatile ("msr tcr_el1, %0" :: "r" ((uint64_t)TCR_VALUE));
    __asm__ volatile ("msr ttbr0_el1, %0" :: "r" ((uint64_t)as->as_root));
    __asm__ volatile ("isb");

    /*
     * The instruction cache may hold lines fetched with translation
     * off. They are lines of the same instructions at the same
     * addresses, because the map is identity, so this is belt and
     * braces rather than a fix for anything observed. It is cheap and
     * it is the kind of thing that is impossible to debug later: a
     * stale instruction line survives every print anyone adds.
     */
    __asm__ volatile ("ic iallu");
    __asm__ volatile ("dsb ish");
    __asm__ volatile ("isb");

    md_tlb_flush_all();

    /*
     * The instruction after the one that turns translation on is fetched
     * through the tables it just installed. Identity mapping is what makes
     * that survivable: the address the pipeline is already working on
     * means the same thing on both sides of this line.
     */
    __asm__ volatile ("mrs %0, sctlr_el1" : "=r" (sctlr));
    sctlr |= SCTLR_M | SCTLR_C | SCTLR_I;
    __asm__ volatile ("msr sctlr_el1, %0" :: "r" (sctlr));
    __asm__ volatile ("isb");
}

void
md_tlb_flush_all(void)
{
    __asm__ volatile ("dsb ishst");
    __asm__ volatile ("tlbi vmalle1is");
    __asm__ volatile ("dsb ish");
    __asm__ volatile ("isb");
}

#endif /* SYS_TIER == 2 */
