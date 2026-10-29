/* kline.h : the kernel's embedded source-line table.
 *
 * SPDX-License-Identifier: 0BSD OR Apache-2.0
 * Copyright (C) 2026 Jon Mayo <jon@rm-f.net>
 *
 * The format of the line blob the mkline host tool builds from the kernel's
 * first-pass ELF and the second-pass link embeds, and the lookup the kernel
 * does against it. It maps a kernel text address to a source file and line.
 *
 * A full line table is one row per statement, which for this kernel is tens
 * of thousands of rows, so the rows are delta encoded: sorted by address,
 * each row carries the address, line and file as a difference from the row
 * before, as LEB128. The reader walks the stream, since a panic is not a hot
 * path and the alternative is a flat table the size of the text.
 *
 * The layout is the header, then one byte offset per file into the string
 * table, then the string table of file base names, then the encoded rows.
 * Included by the kernel and by mkline so the writer and the reader agree.
 *
 */

#ifndef _SYS_KLINE_H_
#define _SYS_KLINE_H_

#include <stdint.h>

struct kline_hdr {
    uint32_t    count;          /* encoded rows that follow */
    uint32_t    filecount;      /* file name offsets after the header */
    uint32_t    strsize;        /* bytes of the file-name string table */
    uint32_t    streamsize;     /* bytes of the encoded rows */
};

/*
 * The source file and line a kernel text address falls on, or zero when no
 * table is embedded or no row precedes the address. The file is a base name
 * and the line is the source line of the nearest row at or below the address.
 */
int kline_lookup(uintptr_t addr, const char **file, int *line);

#endif /* _SYS_KLINE_H_ */
