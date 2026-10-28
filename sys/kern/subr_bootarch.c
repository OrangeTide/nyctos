/* subr_bootarch.c : the boot archive reader and RAM loader.
 *
 * SPDX-License-Identifier: 0BSD OR Apache-2.0
 * Copyright (C) 2026 Jon Mayo <jon@rm-f.net>
 *
 * Looks up members of the boot archive by name and checks each member's
 * layout once per boot. Copies a member to the address it was linked
 * for, with no relocation or symbol resolution, and its entry point is
 * the first byte of the image. Also reports the archive, reserves the
 * memory its images occupy, and gives the bounds of the user window.
 *
 */

#include <sys/bootarch.h>
#include <sys/elf.h>
#include <sys/page.h>
#include <sys/op.h>
#include <sys/systm.h>

#include <stdbool.h>
#include <stdint.h>

extern const uint8_t boot_archive[];
extern const uint8_t boot_archive_end[];

/*
 * Whether every member's layout has been checked. Once per boot: the
 * archive does not change, and the check is a loop over every member on
 * a path taken for every lookup.
 */
static bool layouts_checked;

static const struct boot_header *
archive(void)
{
    const struct boot_header *h = (const struct boot_header *)boot_archive;

    if (h->bh_magic != BOOT_MAGIC) {
        panic("boot: archive magic is 0x%lx, not a boot archive",
          (unsigned long)h->bh_magic);
    }
    if (h->bh_version != BOOT_VERSION) {
        panic("boot: archive version %lu, expected %lu",
          (unsigned long)h->bh_version, (unsigned long)BOOT_VERSION);
    }
    if (h->bh_bytes > (uint32_t)(boot_archive_end - boot_archive)) {
        panic("boot: archive claims %lu bytes, image holds %lu",
          (unsigned long)h->bh_bytes,
          (unsigned long)(boot_archive_end - boot_archive));
    }

    /*
     * And every member's layout, once, the first time anything asks the
     * archive anything. Here rather than in the report, which runs long
     * after the first program has loaded, and rather than per load,
     * which would say nothing about the members nothing loads. What a
     * bad layout deserves is to stop the boot at the description.
     */
    if (!layouts_checked) {
        const struct boot_member *m =
          (const struct boot_member *)(boot_archive + sizeof(*h));
        uint32_t i;

        layouts_checked = true;
        for (i = 0; i < h->bh_count; i++) {
            boot_check_layout(&m[i]);
        }
    }

    return h;
}

/*
 * Compare against a fixed-width name field. The field is padded with NULs
 * and is not required to be terminated, so the comparison stops at the
 * width rather than at a terminator the archive need not contain.
 */
static bool
name_is(const char *field, const char *want)
{
    uint32_t i;

    for (i = 0; i < BOOT_NAME_MAX; i++) {
        if (field[i] != want[i]) {
            return false;
        }
        if (want[i] == '\0') {
            return true;
        }
    }

    return want[BOOT_NAME_MAX] == '\0';
}

const struct boot_member *
boot_lookup(const char *name)
{
    const struct boot_header *h = archive();
    const struct boot_member *m;
    uint32_t i;

    m = (const struct boot_member *)(boot_archive + sizeof(*h));
    for (i = 0; i < h->bh_count; i++) {
        if (name_is(m[i].bm_name, name)) {
            return &m[i];
        }
    }

    return NULL;
}


/*
 * The layout the archive carries for a member, checked rather than
 * trusted.
 *
 * The numbers come from a symbol table by way of a build, and a build
 * that half changed is the way they go wrong: a linker script without
 * the symbols, a tool with an older record format, an image rebuilt
 * while its layout file was not. Every one of those produces a plausible
 * pair of numbers, and a loader that mapped by them would put a
 * program's text where its stack should be.
 *
 * Called once over every member the first time anything asks the
 * archive anything, which is before the first program loads, so that a
 * bad archive stops the boot at the description rather than at whichever
 * program happens to be mapped by it first. The report calls it again,
 * which costs nothing and keeps the two in step.
 */
void
boot_check_layout(const struct boot_member *m)
{
    if ((m->bm_rw % PAGE_SIZE) != 0 || (m->bm_mem % PAGE_SIZE) != 0) {
        panic("boot: member '%s' has a layout off a page boundary: "
          "rw 0x%lx, mem 0x%lx", m->bm_name, (unsigned long)m->bm_rw,
          (unsigned long)m->bm_mem);
    }
    if (m->bm_rw == 0) {
        panic("boot: member '%s' has no read-only region", m->bm_name);
    }
    if (m->bm_mem <= m->bm_rw) {
        panic("boot: member '%s' ends at 0x%lx, at or below where its "
          "writable memory begins", m->bm_name, (unsigned long)m->bm_mem);
    }
    if (m->bm_size > m->bm_mem) {
        panic("boot: member '%s' carries %lu bytes and asks for %lu",
          m->bm_name, (unsigned long)m->bm_size,
          (unsigned long)m->bm_mem);
    }
}

/*
 * Fill a run of a member's image, the bytes at virtual-address offsets
 * [voff, voff+len), into dst. The payload is the member's ELF; its loadable
 * segments carry the bytes, each at the offset its virtual address gives
 * from bm_load. The span is zeroed first and each segment overlays the file
 * bytes that fall in it, so a gap between segments, or the tail of a page
 * past a segment's file bytes, reads as zero, which is what the flat image
 * carried there before.
 */
void
boot_copy_image(const struct boot_member *m, size_t voff, void *dst,
  size_t len)
{
    const uint8_t *elf = boot_payload(m);
    const Elf64_Ehdr *eh = (const Elf64_Ehdr *)elf;
    uint8_t *out = dst;
    unsigned int p;

    if (eh->e_ident[EI_MAG0] != ELFMAG0 || eh->e_ident[1] != ELFMAG1 ||
      eh->e_ident[2] != ELFMAG2 || eh->e_ident[3] != ELFMAG3) {
        panic("boot: member '%s' is not an ELF", m->bm_name);
    }

    memset(out, 0, len);

    for (p = 0; p < eh->e_phnum; p++) {
        const Elf64_Phdr *ph = (const Elf64_Phdr *)
          (elf + eh->e_phoff + (uint64_t)p * eh->e_phentsize);
        uint64_t seg, lo, hi;

        if (ph->p_type != PT_LOAD || ph->p_filesz == 0) {
            continue;
        }
        seg = ph->p_vaddr - m->bm_load;         /* image offset of segment */
        lo = seg > voff ? seg : voff;
        hi = seg + ph->p_filesz < voff + len ? seg + ph->p_filesz : voff + len;
        if (lo < hi) {
            memcpy(out + (lo - voff), elf + ph->p_offset + (lo - seg),
              (size_t)(hi - lo));
        }
    }
}

/*
 * The function a virtual address falls in, by name, from the member's ELF
 * symbol table. The section headers locate the symbol table and the string
 * table it names; the function symbol with the greatest value at or below
 * the address is the one that contains it, and its name is returned. NULL
 * when the ELF carries no symbol table or no function precedes the address.
 * This is what a backtrace turns a return address into, and what 0.32 reads
 * the archive for.
 */
const char *
boot_symbolize(const struct boot_member *m, uintptr_t addr)
{
    const uint8_t *elf = boot_payload(m);
    const Elf64_Ehdr *eh = (const Elf64_Ehdr *)elf;
    const Elf64_Sym *syms = NULL;
    const char *strs = NULL;
    const char *best = NULL;
    uint64_t nsyms = 0, best_val = 0;
    unsigned int s;
    uint64_t i;

    for (s = 0; s < eh->e_shnum; s++) {
        const Elf64_Shdr *sh = (const Elf64_Shdr *)
          (elf + eh->e_shoff + (uint64_t)s * eh->e_shentsize);

        if (sh->sh_type == SHT_SYMTAB && sh->sh_entsize != 0) {
            const Elf64_Shdr *str = (const Elf64_Shdr *)
              (elf + eh->e_shoff + (uint64_t)sh->sh_link * eh->e_shentsize);

            syms = (const Elf64_Sym *)(elf + sh->sh_offset);
            nsyms = sh->sh_size / sh->sh_entsize;
            strs = (const char *)(elf + str->sh_offset);
            break;
        }
    }
    if (syms == NULL || strs == NULL) {
        return NULL;
    }

    for (i = 0; i < nsyms; i++) {
        if (ELF64_ST_TYPE(syms[i].st_info) != STT_FUNC) {
            continue;
        }
        if (syms[i].st_value > addr) {
            continue;
        }
        if (best == NULL || syms[i].st_value > best_val) {
            best = strs + syms[i].st_name;
            best_val = syms[i].st_value;
        }
    }

    return best;
}

/*
 * Copy a member to the address it was linked for and return its entry
 * point. Nothing checks that the destination is free, because there is no
 * MMU and no allocator to ask: the load addresses come from the program
 * Makefiles and not overlapping is their job, per share/mk/nyc.prog.mk.
 */
uintptr_t
boot_load(const struct boot_member *m)
{
    (void)archive();

    if (m->bm_load == 0) {
        panic("boot: member '%s' has no load address", m->bm_name);
    }

    boot_copy_image(m, 0, (void *)(uintptr_t)m->bm_load, m->bm_size);

    return (uintptr_t)m->bm_load;
}

void
boot_report(void)
{
    const struct boot_header *h = archive();
    const struct boot_member *m;
    uint32_t i;

    m = (const struct boot_member *)(boot_archive + sizeof(*h));
    cnprintf("boot archive: %lu members, %lu bytes\n",
      (unsigned long)h->bh_count, (unsigned long)h->bh_bytes);
    for (i = 0; i < h->bh_count; i++) {
        boot_check_layout(&m[i]);

        /*
         * What it needs against what a slot is, in one line per image.
         * The two page counts are the regions the loader will map; the
         * third is what a process costs today.
         */
        cnprintf("    %-12s %4lu bytes, loads at 0x%lx, %lu+%lu of %lu "
          "pages\n", m[i].bm_name, (unsigned long)m[i].bm_size,
          (unsigned long)m[i].bm_load,
          (unsigned long)(m[i].bm_rw / PAGE_SIZE),
          (unsigned long)((m[i].bm_mem - m[i].bm_rw) / PAGE_SIZE),
          (unsigned long)(SYS_USER_SLOT_BYTES / PAGE_SIZE));
    }
}

/*
 * The interface says how long an image name may be and the archive format
 * says how long one is. They are the same number, and a build where they
 * are not would truncate a name in one place and not the other.
 */
_Static_assert(SYS_IMAGE_NAME_MAX == BOOT_NAME_MAX,
  "the spawn interface and the boot archive must agree on name length");

/*
 * Hold back the memory the archive's programs are linked to occupy.
 *
 * A member is loaded at the address it was linked for and runs there, and
 * everything it needs beyond its image, which is its zeroed data and its
 * stack, is inside the slot the build gave it. So the unit to hold is the
 * slot rather than the image: the image is what the archive carries and
 * the slot is what the program uses.
 *
 * Without this the frame allocator hands out the text of a running
 * program, which was true the moment it existed and would have been found
 * out by the first page table that landed on one. At Tier 2 the slots go
 * with it.
 */
void
boot_reserve(void)
{
    const struct boot_header *h = (const struct boot_header *)boot_archive;
    uintptr_t wbase, pa;
    size_t wlen;

    if (h->bh_magic != BOOT_MAGIC) {
        return;
    }

    boot_user_window(&wbase, &wlen);

    /*
     * The whole window, not just the slots inside it. It is rounded up
     * to a two megabyte boundary so that the kernel's map can leave a
     * hole for it made of the same blocks as everything else, and the
     * rounding adds frames that belong to no slot: free, and outside
     * every identity map, which is the worst combination there is.
     * The allocator would hand one out and the kernel would touch it
     * at an address nothing translates.
     */
    for (pa = wbase; pa < wbase + wlen; pa += PAGE_SIZE) {
        if (!page_take(pa)) {
            panic("boot: %p is spoken for and the window needs it",
              (void *)pa);
        }
    }

    cnprintf("  %u slots in a %zu MiB window held for loaded programs\n",
      h->bh_count, wlen / (1024 * 1024));
}

/* Where the first member is linked to run, for a caller checking that the
 * memory a program occupies is not in the free map. */
uintptr_t
boot_first_load(void)
{
    const struct boot_header *h = (const struct boot_header *)boot_archive;
    const struct boot_member *m;

    if (h->bh_magic != BOOT_MAGIC || h->bh_count == 0) {
        return 0;
    }
    m = (const struct boot_member *)(boot_archive + sizeof(*h));

    return (uintptr_t)m[0].bm_load;
}

/*
 * The span of addresses loaded programs occupy.
 *
 * At Tier 0 these are physical addresses and the frame allocator holds
 * them back, because a program runs where it was linked. At Tier 2 they
 * are virtual addresses and every process has its own memory behind them,
 * which is what makes two copies of one image possible there; the frames
 * stay held back all the same, so that every frame the kernel allocates
 * is one it can still reach by its own address.
 *
 * Rounded up to two megabytes because the kernel's map of everything else
 * is made of two megabyte blocks, and a hole that does not line up with
 * them would have to be cut out of one.
 */
void
boot_user_window(uintptr_t *base, size_t *len)
{
    const struct boot_header *h = (const struct boot_header *)boot_archive;
    const struct boot_member *m;
    uintptr_t lo = 0, hi = 0;
    uint32_t i;

    *base = 0;
    *len = 0;

    if (h->bh_magic != BOOT_MAGIC || h->bh_count == 0) {
        return;
    }

    m = (const struct boot_member *)(boot_archive + sizeof(*h));
    lo = (uintptr_t)m[0].bm_load;
    hi = lo;
    for (i = 0; i < h->bh_count; i++) {
        uintptr_t end = (uintptr_t)m[i].bm_load + SYS_USER_SLOT_BYTES;

        if ((uintptr_t)m[i].bm_load < lo) {
            lo = (uintptr_t)m[i].bm_load;
        }
        if (end > hi) {
            hi = end;
        }
    }

    lo &= ~((uintptr_t)0x200000 - 1);
    hi = (hi + 0x200000 - 1) & ~((uintptr_t)0x200000 - 1);

    *base = lo;
    *len = hi - lo;
}

/*
 * Where a member's bytes are, for a caller that copies them somewhere
 * other than the address they were linked for. Tier 2 does exactly that:
 * the address is virtual and the memory behind it is frames of that
 * process's own.
 */
const uint8_t *
boot_payload(const struct boot_member *m)
{
    const struct boot_header *h = (const struct boot_header *)boot_archive;

    if (m->bm_offset + m->bm_elfsize > h->bh_bytes) {
        panic("boot: member '%s' runs past the end of the archive",
          m->bm_name);
    }

    return (const uint8_t *)(boot_archive + m->bm_offset);
}
