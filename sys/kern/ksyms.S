/* ksyms.S : the kernel's function-symbol blob embedded in the image.
 *
 * SPDX-License-Identifier: 0BSD OR Apache-2.0
 * Copyright (C) 2026 Jon Mayo <jon@rm-f.net>
 *
 * Places the symbol blob in read-only data between ksym_blob and
 * ksym_blob_end, using .incbin on KSYM_BLOB_PATH. sys/Makefile builds the
 * blob from the first-pass kernel ELF and supplies the path, then links
 * this object in the second pass. The blob sits in rodata, past the text,
 * so the function addresses it records do not move between the passes.
 *
 */

.section .rodata
.balign 8
.global ksym_blob
ksym_blob:
    .incbin KSYM_BLOB_PATH
.global ksym_blob_end
ksym_blob_end:
