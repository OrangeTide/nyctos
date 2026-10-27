/* subr_page.c : the physical page frame allocator.
 *
 * SPDX-License-Identifier: 0BSD OR Apache-2.0
 * Copyright (C) 2026 Jon Mayo <jon@rm-f.net>
 *
 * Allocates and frees page frames from a statically sized bitmap,
 * scanning from the lowest frame and skipping full words. Frames can
 * also be taken at a given physical address. Reports the frame count,
 * the used count, the high-water mark and per-core totals. A machine
 * that offers more frames than SYS_MAX_FRAMES is capped at boot, with
 * a message.
 *
 */

#include <sys/cpu.h>
#include <sys/intr.h>
#include <sys/lock.h>
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
 * Exclusion, which this did not have.
 *
 * Every other shared structure in the kernel takes a lock at Tier 2:
 * the pools, the object references, the timer list and the console.
 * The frame allocator did not, and disabling interrupts is exclusion on
 * one core and nothing at all on two. It was very nearly true that
 * nothing noticed, because until M13 stage 2 the only caller that freed
 * a frame was a boot check running on one core.
 *
 * That stage is what made it matter. A process is reaped from
 * thread_reap_parked at the top of a switch, which happens on whichever
 * core the switch is on, so freeing a slot now runs on either core
 * while the other may be loading a program. Two cores in this bitmap at
 * once lose a bit, which is one frame handed to two owners, or lose a
 * count, which is the number this whole milestone is measured in.
 *
 * A leaf, like the pool lock and for the same reason: nothing under it
 * takes another lock, so it cannot be half of a cycle.
 */
#if SYS_TIER == 2

static struct spinlock page_lock = { 0, 0, "frames" };

/*
 * How many times each core has been in here, for the same reason the
 * pools count it: a second core that never touched a frame would leave
 * every number correct for the wrong reason and the lock untested.
 */
static uint64_t page_ops[SYS_MAX_CORES];

static intr_state_t
page_enter(void)
{
    intr_state_t s = md_intr_disable();

    spin_lock(&page_lock);
    page_ops[cpu_self()->cp_index]++;

    return s;
}

static void
page_leave(intr_state_t s)
{
    spin_unlock(&page_lock);
    md_intr_restore(s);
}

#else

/*
 * Section 13.3 buys Tier 0 a kernel with no internal locking at all,
 * and one core is what makes disabling interrupts exclusion again.
 */
static intr_state_t
page_enter(void)
{
    return md_intr_disable();
}

static void
page_leave(intr_state_t s)
{
    md_intr_restore(s);
}

#endif /* SYS_TIER == 2 */

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
    intr_state_t s = page_enter();
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
            page_leave(s);

            memset((void *)pa, 0, PAGE_SIZE);

            return pa;
        }
    }

    page_leave(s);

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
    intr_state_t s = page_enter();
    uint32_t i;

    if ((pa & PAGE_MASK) != 0 || pa < frame_base) {
        page_leave(s);
        return false;
    }
    i = (uint32_t)((pa - frame_base) / PAGE_SIZE);
    if (i >= frame_count || taken(i)) {
        page_leave(s);
        return false;
    }

    take(i);
    frame_used++;
    if (frame_used > frame_highwater) {
        frame_highwater = frame_used;
    }

    page_leave(s);

    return true;
}

void
page_free(uintptr_t pa)
{
    intr_state_t s;
    uint32_t i;

    /*
     * Before the lock, because neither reads anything another core
     * could be changing, and a panic holding a lock is a panic that
     * has to be reasoned about.
     */
    if ((pa & PAGE_MASK) != 0 || pa < frame_base) {
        panic("page: freeing %p, which is not a frame", (void *)pa);
    }
    i = (uint32_t)((pa - frame_base) / PAGE_SIZE);
    if (i >= frame_count) {
        panic("page: freeing %p, which is outside the map", (void *)pa);
    }

    s = page_enter();

    if (!taken(i)) {
        panic("page: freeing %p, which was already free", (void *)pa);
    }

    give(i);
    frame_used--;

    page_leave(s);
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


/*
 * Which cores have taken frames and given them back.
 *
 * The same question the pools answer about themselves, and here it has
 * a second use. M13 stage 2 freed a process's memory from the reap, and
 * a reap runs at the top of whichever core's switch collects the parked
 * thread. Whether that is really both cores is not something the design
 * can assert on its own, and a lock nothing contends is a lock that has
 * never been tried.
 *
 * It says so rather than insisting. pool_report_cores halts a boot in
 * which a core never touched a pool, and the same rule here would halt
 * every boot there is: measured, every entry to this allocator happens
 * on the core that runs the boot thread, at two cores and at four. The
 * lock is still right, because a fault serviced by a pager or a spawn
 * made by a program is this allocator entered from wherever that
 * program runs. It is untested by contention, and printing zero is how
 * that stays visible.
 */
void
page_report_cores(void)
{
#if SYS_TIER == 2
    uint64_t total = 0;
    uint32_t i;

    for (i = 0; i < cpu_count(); i++) {
        total += page_ops[i];
    }

    cnprintf("frames: taken and given back %lu times,",
      (unsigned long)total);
    for (i = 0; i < cpu_count(); i++) {
        cnprintf("%s %lu on core %u", i == 0 ? "" : " and",
          (unsigned long)page_ops[i], (unsigned int)i);
    }
    cnprintf("\n");
#endif
}
