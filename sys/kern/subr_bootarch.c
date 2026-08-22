/*
 * SPDX-License-Identifier: 0BSD OR Apache-2.0
 * Copyright (C) 2026 Jon Mayo <jon@rm-f.net>
 */

/*
 * Walking the boot archive, and loading a member out of it.
 *
 * Section 24.4 describes this as a loop over fixed-size records, on the
 * order of forty lines, and not a filesystem: it speaks no protocol and
 * resolves no paths. That is what this is.
 *
 * Section 15.2 makes execute-in-place the Tier 0 default and a RAM loader
 * a compile-time alternative for images that must run from RAM. This is
 * the RAM loader, and it is the only path here because the programs are
 * linked at absolute addresses. Running in place would need them built
 * position-independent, which is a change to how they are compiled rather
 * than to anything in this file.
 *
 * The copy is therefore the whole of loading: no relocation, no symbol
 * resolution, no segment table. A flat binary goes to the address it was
 * linked for and the entry point is the first byte of it, because the
 * linker script places .text.start first.
 */

#include <sys/bootarch.h>
#include <sys/page.h>
#include <sys/op.h>
#include <sys/systm.h>

#include <stdbool.h>
#include <stdint.h>

extern const uint8_t boot_archive[];
extern const uint8_t boot_archive_end[];

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
 * Copy a member to the address it was linked for and return its entry
 * point. Nothing checks that the destination is free, because there is no
 * MMU and no allocator to ask: the load addresses come from the program
 * Makefiles and not overlapping is their job, per share/mk/nyc.prog.mk.
 * Section 15.2 is where a loader service takes ownership of that.
 */
uintptr_t
boot_load(const struct boot_member *m)
{
    const struct boot_header *h = archive();

    if (m->bm_offset + m->bm_size > h->bh_bytes) {
        panic("boot: member '%s' runs past the end of the archive",
          m->bm_name);
    }
    if (m->bm_load == 0) {
        panic("boot: member '%s' has no load address", m->bm_name);
    }

    memcpy((void *)(uintptr_t)m->bm_load, boot_archive + m->bm_offset,
      m->bm_size);

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
        cnprintf("    %-16s %lu bytes, loads at 0x%lx\n", m[i].bm_name,
          (unsigned long)m[i].bm_size, (unsigned long)m[i].bm_load);
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
 * out by the first page table that landed on one. Section 15.2's loader is
 * what ends the whole arrangement, and at Tier 2 the slots go with it.
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

    if (m->bm_offset + m->bm_size > h->bh_bytes) {
        panic("boot: member '%s' runs past the end of the archive",
          m->bm_name);
    }

    return (const uint8_t *)(boot_archive + m->bm_offset);
}
