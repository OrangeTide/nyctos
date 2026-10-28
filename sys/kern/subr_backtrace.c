/* subr_backtrace.c : the machine-independent stack unwinder.
 *
 * SPDX-License-Identifier: 0BSD OR Apache-2.0
 * Copyright (C) 2026 Jon Mayo <jon@rm-f.net>
 *
 * Walks the saved frame-pointer chain and collects the return address of
 * each frame. One frame is decoded by the architecture; this follows the
 * chain and guards it, so a frame pointer that is zero, not above the one
 * before it, or misaligned ends the walk rather than reading somewhere it
 * should not.
 *
 */

#include <sys/backtrace.h>
#include <sys/config.h>
#include <sys/ksym.h>

#include <stddef.h>
#include <stdint.h>

/*
 * The symbol blob the link embeds, between these symbols. The first pass
 * embeds an empty blob, a header saying zero functions, so a lookup finds
 * nothing but the references still resolve; the second pass embeds the real
 * one built from the first.
 */
extern const uint8_t ksym_blob[];
extern const uint8_t ksym_blob_end[];

const char *
ksym_lookup(uintptr_t addr, uintptr_t *off)
{
    const struct ksym_hdr *h = (const struct ksym_hdr *)ksym_blob;
    const struct ksym_ent *ent;
    const char *strs;
    const char *best = NULL;
    uint64_t best_addr = 0;
    uint32_t i;

    if (ksym_blob == ksym_blob_end) {
        return NULL;
    }

    ent = (const struct ksym_ent *)(ksym_blob + sizeof(*h));
    strs = (const char *)(ent + h->count);
    for (i = 0; i < h->count; i++) {
        if (ent[i].addr > addr) {
            continue;
        }
        if (best == NULL || ent[i].addr > best_addr) {
            best = strs + ent[i].name;
            best_addr = ent[i].addr;
        }
    }

    if (best == NULL) {
        return NULL;
    }
    if (off != NULL) {
        *off = addr - best_addr;
    }

    return best;
}

unsigned int
kbacktrace(uintptr_t *out, unsigned int max)
{
    uintptr_t fp = (uintptr_t)__builtin_frame_address(0);
    uintptr_t base = fp;
    unsigned int n = 0;

    /*
     * The frame pointer stays within one stack above where the walk began,
     * so a chain that does not terminate cleanly stops at the stack's edge
     * rather than reading an address that is not stack. With the per-frame
     * alignment and the requirement that each caller's frame is above this
     * one, every address read is inside the stack this call runs on.
     */
    while (n < max && fp != 0 && (fp & (sizeof(uintptr_t) - 1)) == 0 &&
      fp - base < SYS_KSTACK_SIZE) {
        uintptr_t ra = 0, caller = 0;

        if (!md_frame_step(fp, &ra, &caller)) {
            break;
        }
        if (ra == 0) {
            break;
        }
        out[n++] = ra;

        /* The stack grows down, so each caller's frame is above this one. */
        if (caller <= fp) {
            break;
        }
        fp = caller;
    }

    return n;
}
