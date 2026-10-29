/* klines.S : the kernel's source-line blob embedded in the image.
 *
 * SPDX-License-Identifier: 0BSD OR Apache-2.0
 * Copyright (C) 2026 Jon Mayo <jon@rm-f.net>
 *
 * Places the line blob in read-only data between kline_blob and
 * kline_blob_end, using .incbin on KLINE_BLOB_PATH. sys/Makefile builds the
 * blob from the first-pass kernel ELF and supplies the path, then links this
 * object in the second pass. The blob sits in rodata, past the text, so the
 * text addresses it records do not move between the passes.
 *
 */

.section .rodata
.balign 8
.global kline_blob
kline_blob:
    .incbin KLINE_BLOB_PATH
.global kline_blob_end
kline_blob_end:
