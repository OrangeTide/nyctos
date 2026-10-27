/* bootimg.S : the boot archive embedded in the kernel image.
 *
 * SPDX-License-Identifier: 0BSD OR Apache-2.0
 * Copyright (C) 2026 Jon Mayo <jon@rm-f.net>
 *
 * Places the boot archive in read-only data between the boot_archive and
 * boot_archive_end symbols, using .incbin on BOOT_ARCHIVE_PATH. sys/Makefile
 * supplies that path and builds the archive from the programs under bin/.
 *
 */

.section .rodata
.balign 4096
.global boot_archive
boot_archive:
    .incbin BOOT_ARCHIVE_PATH
.global boot_archive_end
boot_archive_end:
