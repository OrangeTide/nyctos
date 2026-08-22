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

static uint64_t *
descend(uint64_t *table, uint32_t index)
{
    uintptr_t pa;

    if ((table[index] & PTE_V) != 0) {
        if (PTE_LEAF(table[index])) {
            panic("pmap: descending into a leaf at index %u", index);
        }
        return table_at((uintptr_t)PTE_ADDR(table[index]));
    }

    pa = page_alloc();
    if (pa == 0) {
        return NULL;
    }
    table[index] = PTE_PPN(pa) | PTE_V;

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
    uint64_t *l0 = table_at(as->as_root);

    if ((va & PAGE_MASK) != 0 || (pa & PAGE_MASK) != 0 ||
      (len & PAGE_MASK) != 0) {
        return SYS_ERR_INVALID_ARG;
    }

    while (len > 0) {
        uint64_t *l1 = descend(l0, (uint32_t)INDEX(va, 0));
        size_t step;

        if (l1 == NULL) {
            return SYS_ERR_NO_MEMORY;
        }

        /* A megapage where the range allows one; see the other port. */
        if (len >= LEVEL_SIZE(1) && (va & (LEVEL_SIZE(1) - 1)) == 0 &&
          (pa & (LEVEL_SIZE(1) - 1)) == 0) {
            l1[INDEX(va, 1)] = PTE_PPN(pa) | leaf_attrs(flags);
            step = LEVEL_SIZE(1);
        } else {
            uint64_t *l2 = descend(l1, (uint32_t)INDEX(va, 1));

            if (l2 == NULL) {
                return SYS_ERR_NO_MEMORY;
            }
            l2[INDEX(va, 2)] = PTE_PPN(pa) | leaf_attrs(flags);
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
     * The test finisher, which is how this board is switched off. It
     * is one register in the first megabyte of the address space and
     * it was the last thing left unmapped: the whole session ran and
     * then faulted on the store that ends the machine, which is a
     * good demonstration that a device nobody uses until the end is a
     * device nobody notices is missing.
     */
    err = md_map(as, 0x00100000UL, 0x00100000UL, 0x00200000UL,
      VM_READ | VM_WRITE | VM_DEVICE);
    if (err != SYS_OK) {
        return err;
    }

    /* The PLIC, which is four megabytes of it, and the 16550 above it. */
    err = md_map(as, 0x0c000000UL, 0x0c000000UL, 0x00400000UL,
      VM_READ | VM_WRITE | VM_DEVICE);
    if (err != SYS_OK) {
        return err;
    }

    /*
     * The 16550, reachable from user mode, which is a deviation with
     * a stage against it: the console driver holds its registers as
     * an address compiled into it, per section 19.2's grant that does
     * not exist yet. Stage 7 hands it a grant and this flag goes.
     */
    return md_map(as, 0x10000000UL, 0x10000000UL, 0x00200000UL,
      VM_READ | VM_WRITE | VM_DEVICE | VM_USER);
}

void
md_as_activate(const struct addrspace *as)
{
    uint64_t satp = SATP_SV39 | ((uint64_t)as->as_root >> 12);

    /*
     * Supervisor mode may not touch a page marked for user mode
     * unless SUM says it may. The kernel has to: it reads an
     * operation's arguments out of a program's memory, copies a
     * message payload, and at this stage its own data is marked user
     * because a program's startup record is inside it. Without this
     * bit the kernel faults on its own stack, which is what happened:
     * a load fault, then a store fault trying to save the frame for
     * the first one, and so on until the machine stopped.
     *
     * It is a permission the kernel gives itself, not a check it
     * skips. Section 23.2's validation is what says an address
     * belongs to the caller, and stage 4 is where that arrives.
     */
    __asm__ volatile ("csrs sstatus, %0" :: "r" (1UL << 18) : "memory");

    /*
     * The fence after the write is what makes the instruction after it
     * fetch through the new tables rather than through whatever the
     * hardware had cached about there being none. Identity mapping is what
     * makes that survivable.
     */
    __asm__ volatile ("csrw satp, %0" :: "r" (satp) : "memory");
    __asm__ volatile ("sfence.vma" ::: "memory");
}

void
md_tlb_flush_all(void)
{
    __asm__ volatile ("sfence.vma" ::: "memory");
}

#endif /* SYS_TIER == 2 */
