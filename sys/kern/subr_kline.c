/* subr_kline.c : name a kernel address with its source line.
 *
 * SPDX-License-Identifier: 0BSD OR Apache-2.0
 * Copyright (C) 2026 Jon Mayo <jon@rm-f.net>
 *
 * Reads the line blob the link embeds and turns a text address into the
 * source file and line the mkline host tool recorded for it. The blob is
 * delta encoded, sorted by address, so the lookup walks the rows and keeps
 * the last one at or below the address. A panic is not a hot path, so the
 * walk is the cost of a table small enough to carry.
 *
 */

#include <sys/kline.h>

#include <stddef.h>
#include <stdint.h>

/*
 * The line blob the link embeds, between these symbols. The first pass
 * embeds an empty blob, a header saying zero rows, so a lookup finds nothing
 * but the references still resolve; the second pass embeds the real one
 * built from the first.
 */
extern const uint8_t kline_blob[];
extern const uint8_t kline_blob_end[];

static uint64_t
rd_uleb(const uint8_t **p, const uint8_t *end)
{
    uint64_t v = 0;
    int shift = 0;
    uint8_t b;

    do {
        if (*p >= end) {
            break;
        }
        b = *(*p)++;
        if (shift < 64) {
            v |= (uint64_t)(b & 0x7f) << shift;
        }
        shift += 7;
    } while (b & 0x80);

    return v;
}

static int64_t
rd_sleb(const uint8_t **p, const uint8_t *end)
{
    int64_t v = 0;
    int shift = 0;
    uint8_t b = 0;

    do {
        if (*p >= end) {
            break;
        }
        b = *(*p)++;
        if (shift < 64) {
            v |= (int64_t)(b & 0x7f) << shift;
        }
        shift += 7;
    } while (b & 0x80);
    if (shift < 64 && (b & 0x40)) {
        v |= -((int64_t)1 << shift);
    }

    return v;
}

int
kline_lookup(uintptr_t addr, const char **file, int *line)
{
    const struct kline_hdr *h = (const struct kline_hdr *)kline_blob;
    const uint32_t *foff;
    const char *strs;
    const uint8_t *p, *end;
    uint64_t cur = 0;
    int64_t cur_line = 0, cur_file = 0;
    int64_t best_line = 0, best_file = 0;
    uint32_t i;
    int found = 0;

    if (kline_blob == kline_blob_end || h->count == 0) {
        return 0;
    }

    foff = (const uint32_t *)(kline_blob + sizeof(*h));
    strs = (const char *)(foff + h->filecount);
    p = (const uint8_t *)(strs + h->strsize);
    end = p + h->streamsize;

    for (i = 0; i < h->count && p < end; i++) {
        cur += rd_uleb(&p, end);
        cur_line += rd_sleb(&p, end);
        cur_file += rd_sleb(&p, end);
        if (cur <= (uint64_t)addr) {
            best_line = cur_line;
            best_file = cur_file;
            found = 1;
        } else {
            break;      /* the rows are sorted, so none past here is below */
        }
    }

    if (!found || best_file < 0 || (uint32_t)best_file >= h->filecount) {
        return 0;
    }
    if (file != NULL) {
        *file = strs + foff[best_file];
    }
    if (line != NULL) {
        *line = (int)best_line;
    }

    return 1;
}
