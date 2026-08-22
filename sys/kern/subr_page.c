/*
 * SPDX-License-Identifier: 0BSD OR Apache-2.0
 * Copyright (C) 2026 Jon Mayo <jon@rm-f.net>
 */

/*
 * The frame allocator. See sys/sys/page.h for what it is and why it is not
 * a pool.
 *
 * A bitmap, scanned from the bottom, skipping words that are full. The
 * first version had a rotating cursor so that a run of allocations did
 * not rescan what it had just handed out, and it went because it made
 * the one property worth checking untrue: with a cursor, a frame that
 * has just been released is not the next one out, and a released frame
 * becoming available again is then only observable at exhaustion.
 *
 * Lowest-first is what section 7.3 refuses for handles, and for a
 * reason that does not apply here: a handle value is a name a program
 * sees and could come to rely on, and a frame number is not. What it
 * costs is a scan, and skipping full words makes that a word per
 * thirty-two frames.
 * The bitmap is statically sized, which keeps this out of the bootstrap
 * problem of needing an allocator to build the allocator. What it costs is
 * a build-time ceiling on how much memory a port may hand over, and a
 * machine with more says so at boot rather than silently using less.
 */

#include <sys/intr.h>
#include <sys/page.h>
#include <sys/systm.h>

#define BITS_PER_WORD   (8u * (uint32_t)sizeof(uint32_t))
#define BITMAP_WORDS    ((SYS_MAX_FRAMES + BITS_PER_WORD - 1) / BITS_PER_WORD)

static uint32_t bitmap[BITMAP_WORDS];
static uintptr_t frame_base;
static uint32_t frame_count;
static uint32_t frame_used;
static uint32_t frame_highwater;

/*
 * What the port asked to be held back, kept so that handing one out
 * can be refused rather than discovered. The kernel image is in it,
 * and a frame inside the kernel image is memory the allocator will
 * zero: the symptom is the kernel's own instructions turning into
 * zeroes underneath it, some milliseconds later, wherever it happens
 * to be executing.
 */
static uintptr_t hold_lo, hold_hi;

static bool
taken(uint32_t i)
{
    return (bitmap[i / BITS_PER_WORD] & (1u << (i % BITS_PER_WORD))) != 0;
}

static void
take(uint32_t i)
{
    bitmap[i / BITS_PER_WORD] |= 1u << (i % BITS_PER_WORD);
}

static void
give(uint32_t i)
{
    bitmap[i / BITS_PER_WORD] &= ~(1u << (i % BITS_PER_WORD));
}

void
page_init(uintptr_t base, size_t len, uintptr_t hold, size_t hold_len)
{
    uintptr_t first = PAGE_ALIGN_UP(base);
    uintptr_t last = PAGE_ALIGN_DOWN(base + len);
    uint32_t i;

    for (i = 0; i < BITMAP_WORDS; i++) {
        bitmap[i] = 0;
    }

    if (last <= first) {
        panic("page: the memory map is empty");
    }

    frame_base = first;
    frame_count = (uint32_t)((last - first) / PAGE_SIZE);
    frame_used = 0;
    frame_highwater = 0;

    /*
     * A machine with more memory than the bitmap covers uses what the
     * bitmap covers and says so. Silently using less is the failure this
     * exists to avoid: a system that has been given twice the memory it
     * uses looks exactly like one that has not.
     */
    if (frame_count > SYS_MAX_FRAMES) {
        cnprintf("page: %u frames offered, %u usable; raise SYS_MAX_FRAMES\n",
          frame_count, (unsigned int)SYS_MAX_FRAMES);
        frame_count = SYS_MAX_FRAMES;
    }

    /*
     * The region held back is the kernel image and the boot archive inside
     * it. Marking it allocated rather than excluding it from the map keeps
     * the arithmetic between a frame number and an address in one place.
     */
    hold_lo = PAGE_ALIGN_DOWN(hold);
    hold_hi = PAGE_ALIGN_UP(hold + hold_len);

    if (hold_len > 0) {
        uintptr_t h = PAGE_ALIGN_DOWN(hold);
        uintptr_t h_end = PAGE_ALIGN_UP(hold + hold_len);

        while (h < h_end) {
            if (h >= frame_base) {
                uint32_t index = (uint32_t)((h - frame_base) / PAGE_SIZE);

                if (index < frame_count && !taken(index)) {
                    take(index);
                    frame_used++;
                }
            }
            h += PAGE_SIZE;
        }
        frame_highwater = frame_used;
    }
}

uintptr_t
page_alloc(void)
{
    intr_state_t s = md_intr_disable();
    uint32_t w;

    for (w = 0; w * BITS_PER_WORD < frame_count; w++) {
        uint32_t bit;

        if (bitmap[w] == 0xffffffffu) {
            continue;
        }
        for (bit = 0; bit < BITS_PER_WORD; bit++) {
            uint32_t i = w * BITS_PER_WORD + bit;
            uintptr_t pa;

            if (i >= frame_count || taken(i)) {
                continue;
            }

            take(i);
            frame_used++;
            if (frame_used > frame_highwater) {
                frame_highwater = frame_used;
            }
            pa = frame_base + (uintptr_t)i * PAGE_SIZE;
            if (pa >= hold_lo && pa < hold_hi) {
                panic("page: handing out %p, inside the kernel image",
                  (void *)pa);
            }
            md_intr_restore(s);

            memset((void *)pa, 0, PAGE_SIZE);

            return pa;
        }
    }

    md_intr_restore(s);

    return 0;
}


/*
 * Reserve one frame by address, for a caller that needs a particular one
 * rather than any one: memory a device occupies, memory an image was
 * linked to sit in, memory somebody else already owns. Returns false if it
 * is taken, which is the answer that matters.
 */
bool
page_take(uintptr_t pa)
{
    intr_state_t s = md_intr_disable();
    uint32_t i;

    if ((pa & PAGE_MASK) != 0 || pa < frame_base) {
        md_intr_restore(s);
        return false;
    }
    i = (uint32_t)((pa - frame_base) / PAGE_SIZE);
    if (i >= frame_count || taken(i)) {
        md_intr_restore(s);
        return false;
    }

    take(i);
    frame_used++;
    if (frame_used > frame_highwater) {
        frame_highwater = frame_used;
    }

    md_intr_restore(s);

    return true;
}

void
page_free(uintptr_t pa)
{
    intr_state_t s = md_intr_disable();
    uint32_t i;

    if ((pa & PAGE_MASK) != 0 || pa < frame_base) {
        panic("page: freeing %p, which is not a frame", (void *)pa);
    }
    i = (uint32_t)((pa - frame_base) / PAGE_SIZE);
    if (i >= frame_count) {
        panic("page: freeing %p, which is outside the map", (void *)pa);
    }
    if (!taken(i)) {
        panic("page: freeing %p, which was already free", (void *)pa);
    }

    give(i);
    frame_used--;

    md_intr_restore(s);
}

uint32_t
page_count(void)
{
    return frame_count;
}

uint32_t
page_used(void)
{
    return frame_used;
}

uint32_t
page_highwater(void)
{
    return frame_highwater;
}

void
page_report(void)
{
    cnprintf("frames: %u of %u free, %u bytes each, from %p\n",
      frame_count - frame_used, frame_count, (unsigned int)PAGE_SIZE,
      (void *)frame_base);
}

/*
 * The last frame in the map, for a caller that wants to find out whether
 * the port's claim about the machine is true.
 */
uintptr_t
page_last(void)
{
    if (frame_count == 0) {
        return 0;
    }

    return frame_base + (uintptr_t)(frame_count - 1) * PAGE_SIZE;
}

