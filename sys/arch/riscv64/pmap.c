/*
 * SPDX-License-Identifier: 0BSD OR Apache-2.0
 * Copyright (C) 2026 Jon Mayo <jon@rm-f.net>
 */

/*
 * Page tables, RISC-V Sv39.
 *
 * Three levels of five hundred and twelve entries: one gigabyte, two
 * megabytes, four kilobytes. The same shape as the other port, which is
 * not a coincidence and is why the surface above them is one function.
 *
 * A leaf is an entry with any of read, write or execute set; a table is an
 * entry with none of them. That is the one rule of this format worth
 * knowing, and it is why a walk here tests permissions rather than a type
 * field.
 */

#include <sys/intr.h>
#include <sys/page.h>
#include <sys/systm.h>
#include <sys/vm.h>

#if SYS_TIER == 2

#define ENTRIES         512
/*
 * Level zero is the root and indexes with VA[38:30]; level two is the
 * leaf and indexes with VA[20:12]. Writing this the other way round,
 * with the root shift a level too high, put every mapping in root
 * entry zero and made a two megabyte range into a leaf at the level
 * where an entry covers a gigabyte. The machine stopped at the
 * instruction that enabled translation with nothing printed.
 */
#define LEVEL_SHIFT(l)  (12 + 9 * (2 - (l)))    /* l = 0, 1, 2 */
#define LEVEL_SIZE(l)   ((uintptr_t)1 << LEVEL_SHIFT(l))
#define INDEX(va, l)    (((va) >> LEVEL_SHIFT(l)) & (ENTRIES - 1))

#define PTE_V           (1ULL << 0)
#define PTE_R           (1ULL << 1)
#define PTE_W           (1ULL << 2)
#define PTE_X           (1ULL << 3)
#define PTE_U           (1ULL << 4)
#define PTE_G           (1ULL << 5)
#define PTE_A           (1ULL << 6)
#define PTE_D           (1ULL << 7)

#define PTE_PPN(pa)     (((uint64_t)(pa) >> 12) << 10)
#define PTE_ADDR(pte)   ((((uint64_t)(pte)) >> 10) << 12)
#define PTE_LEAF(pte)   (((pte) & (PTE_R | PTE_W | PTE_X)) != 0)

#define SATP_SV39       (8ULL << 60)

/*
 * Accessed and dirty are set here rather than left to the hardware. Some
 * parts update them and some raise a fault instead, and a kernel that does
 * not track either has nothing to gain from being told: section 15.1 has
 * no swap and no dirty tracking, so a page that is mapped is a page that
 * is used.
 */
static uint64_t
leaf_attrs(uint32_t flags)
{
    uint64_t e = PTE_V | PTE_A | PTE_D;

    if (flags & VM_READ) {
        e |= PTE_R;
    }
    if (flags & VM_WRITE) {
        e |= PTE_W | PTE_R;
    }
    if (flags & VM_EXEC) {
        e |= PTE_X;
    }
    if (flags & VM_USER) {
        e |= PTE_U;
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
 * A leaf at this level covers a megapage or a gigapage, and is split
 * into the table it stands for: the same memory, the same flags,
 * described one page at a time. Splitting is what lets anything refine a
 * mapping the identity map made out of megapages, which is every grant
 * over ordinary memory and every page a pager will supply.
 *
 * Returns NULL when there is no frame for the table.
 */
static uint64_t *
descend(uint64_t *table, uint32_t index, unsigned int level)
{
    uintptr_t pa;
    uint64_t *next;

    if ((table[index] & PTE_V) != 0 && !PTE_LEAF(table[index])) {
        return table_at((uintptr_t)PTE_ADDR(table[index]));
    }

    pa = page_alloc();
    if (pa == 0) {
        return NULL;
    }
    next = table_at(pa);
    vm_charge_table();

    if ((table[index] & PTE_V) != 0) {
        uint64_t leaf = table[index];
        uintptr_t out = (uintptr_t)PTE_ADDR(leaf);
        uint64_t flags = leaf & 0x3ffULL;
        size_t step = LEVEL_SIZE(level + 1);
        uint32_t i;

        for (i = 0; i < ENTRIES; i++) {
            next[i] = PTE_PPN(out + (uintptr_t)i * step) | flags;
        }
    }

    table[index] = PTE_PPN(pa) | PTE_V;

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
 * take out from under the kernel itself. PTE_LEAF is what tells them
 * apart, and it has to be asked at every level, because the identity
 * map is made of gigapages and megapages and a leaf at level zero is as
 * real as one at level two.
 *
 * The tables are private to this space even though what they describe
 * is not. map_kernel_into calls md_map, and md_map builds its own way
 * down, which is why an address space costs seven frames here and not
 * one.
 */
static void
free_table(uintptr_t pa, unsigned int level)
{
    uint64_t *t = table_at(pa);
    uint32_t i;

    if (level < 2) {
        for (i = 0; i < ENTRIES; i++) {
            if ((t[i] & PTE_V) == 0 || PTE_LEAF(t[i])) {
                continue;
            }
            free_table((uintptr_t)PTE_ADDR(t[i]), level + 1);
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

    free_table(as->as_root, 0);

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
 * The same walk md_as_destroy makes, counting where that one frees. It
 * exists so the ledger at the end of the boot can say what an address
 * space is worth without taking it apart to find out, and having the
 * two walks side by side is what keeps them agreeing about which
 * entries are tables.
 */
static uint32_t
count_table(uintptr_t pa, unsigned int level)
{
    uint64_t *t = table_at(pa);
    uint32_t n = 1;
    uint32_t i;

    if (level < 2) {
        for (i = 0; i < ENTRIES; i++) {
            if ((t[i] & PTE_V) == 0 || PTE_LEAF(t[i])) {
                continue;
            }
            n += count_table((uintptr_t)PTE_ADDR(t[i]), level + 1);
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

    return count_table(as->as_root, 0);
}

sys_err_t
md_map(struct addrspace *as, uintptr_t va, uintptr_t pa, size_t len,
  uint32_t flags)
{
    uint64_t *l0 = table_at(as->as_root);

    if ((va & PAGE_MASK) != 0 || (pa & PAGE_MASK) != 0 ||
      (len & PAGE_MASK) != 0) {
        return SYS_ERR_INVALID_ARG;
    }

    while (len > 0) {
        uint64_t *l1 = descend(l0, (uint32_t)INDEX(va, 0), 0);
        size_t step;

        if (l1 == NULL) {
            return SYS_ERR_NO_MEMORY;
        }

        /* A megapage where the range allows one; see the other port. */
        if (len >= LEVEL_SIZE(1) && (va & (LEVEL_SIZE(1) - 1)) == 0 &&
          (pa & (LEVEL_SIZE(1) - 1)) == 0) {
            l1[INDEX(va, 1)] = PTE_PPN(pa) | leaf_attrs(flags);
            vm_charge_leaf();
            step = LEVEL_SIZE(1);
        } else {
            uint64_t *l2 = descend(l1, (uint32_t)INDEX(va, 1), 1);

            if (l2 == NULL) {
                return SYS_ERR_NO_MEMORY;
            }
            l2[INDEX(va, 2)] = PTE_PPN(pa) | leaf_attrs(flags);
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

    /*
     * The test finisher, one page, which is how this board is switched
     * off. It is one register and was two megabytes, for the same reason
     * everything else here was: the identity map liked the size.
     */
    err = md_map(as, 0x00100000UL, 0x00100000UL, 0x1000UL,
      VM_READ | VM_WRITE | VM_DEVICE);
    if (err != SYS_OK) {
        return err;
    }

    /*
     * The core-local interruptor, one page of it. One word per hart,
     * and writing another hart's word is how a core is poked: section
     * 11.4 has donation cross a boundary by inter-processor interrupt,
     * and this is the device that carries one on this port.
     */
    err = md_map(as, 0x02000000UL, 0x02000000UL, 0x1000UL,
      VM_READ | VM_WRITE | VM_DEVICE);
    if (err != SYS_OK) {
        return err;
    }

    /* The PLIC, which is four megabytes of it. */
    err = md_map(as, 0x0c000000UL, 0x0c000000UL, 0x00400000UL,
      VM_READ | VM_WRITE | VM_DEVICE);
    if (err != SYS_OK) {
        return err;
    }

    /*
     * The 16550, one page. A block is exactly what a grant cannot
     * refine: section 19.2 has a driver given this page with user access
     * in its own space, and a mapping the size of a block would have to
     * be split to allow it.
     *
     * Not reachable from user mode. It was, for three stages, because
     * the driver held the address as a constant and would have faulted
     * on the first register it touched; it holds a grant now, and this
     * is the deviation those stages were carrying.
     */
    return md_map(as, 0x10000000UL, 0x10000000UL, 0x1000UL,
      VM_READ | VM_WRITE | VM_DEVICE);
}

void
md_as_activate(const struct addrspace *as)
{
    uint64_t satp = SATP_SV39 | ((uint64_t)as->as_root >> 12);

    __asm__ volatile ("csrw satp, %0" :: "r" (satp) : "memory");
    __asm__ volatile ("sfence.vma" ::: "memory");
}

void
md_mmu_enable(const struct addrspace *as)
{
    /*
     * Supervisor mode may not touch a page marked for user mode
     * unless SUM says it may, and the kernel has to: it reads an
     * operation's arguments out of a program's memory and copies a
     * message payload. It is a permission the kernel gives itself,
     * not a check it skips; what says an address belongs to the
     * caller is the validation of section 23.2.
     */
    __asm__ volatile ("csrs sstatus, %0" :: "r" (1UL << 18) : "memory");

    md_as_activate(as);

    /*
     * Instructions fetched with translation off are the same
     * instructions at the same addresses, because the map is
     * identity, and a stale one would survive every print anyone
     * added while looking for it.
     */
    __asm__ volatile ("fence.i" ::: "memory");
}

void
md_tlb_flush_all(void)
{
    vm_charge_flush();

    __asm__ volatile ("sfence.vma" ::: "memory");
}

/*
 * The two operand form: an address in the first, and zero in the second
 * for every address space identifier. This port assigns none, so every
 * translation it makes lives under the same one, and zero is the only
 * honest thing to put there.
 */
void
md_tlb_invalidate(uintptr_t va)
{
    vm_charge_inval();

    __asm__ volatile ("sfence.vma %0, zero" :: "r" (va) : "memory");
}


/*
 * What a virtual address names, for a caller handing a page to another
 * address space. Only a page-granule mapping answers: a megapage covers
 * more than the caller asked about, and the ranges this is used on are
 * program memory, which is mapped a page at a time.
 */
sys_err_t
md_lookup(const struct addrspace *as, uintptr_t va, uintptr_t *out_pa)
{
    uint64_t *l0 = table_at(as->as_root);
    uint64_t e;

    e = l0[INDEX(va, 0)];
    if ((e & PTE_V) == 0 || PTE_LEAF(e)) {
        return SYS_ERR_INVALID_ARG;
    }
    e = table_at((uintptr_t)PTE_ADDR(e))[INDEX(va, 1)];
    if ((e & PTE_V) == 0 || PTE_LEAF(e)) {
        return SYS_ERR_INVALID_ARG;
    }
    e = table_at((uintptr_t)PTE_ADDR(e))[INDEX(va, 2)];
    if ((e & PTE_V) == 0) {
        return SYS_ERR_INVALID_ARG;
    }

    *out_pa = (uintptr_t)PTE_ADDR(e);

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
    uint64_t *l0 = table_at(as->as_root);

    while (len > 0) {
        uint64_t e = l0[INDEX(va, 0)];

        if ((e & PTE_V) != 0 && !PTE_LEAF(e)) {
            e = table_at((uintptr_t)PTE_ADDR(e))[INDEX(va, 1)];
            if ((e & PTE_V) != 0 && !PTE_LEAF(e)) {
                table_at((uintptr_t)PTE_ADDR(e))[INDEX(va, 2)] = 0;
                vm_charge_leaf();
            }
        }

        va += PAGE_SIZE;
        len -= PAGE_SIZE;
    }
}
#endif /* SYS_TIER == 2 */
