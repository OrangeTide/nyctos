/*
 * SPDX-License-Identifier: 0BSD OR Apache-2.0
 * Copyright (C) 2026 Jon Mayo <jon@rm-f.net>
 */

/*
 * The boot archive, carried inside the kernel image.
 *
 * Section 24.4 has root walk an archive to find the programs it spawns.
 * There is no root process yet and no storage to read an archive from, so
 * it travels in the kernel's own rodata, which is the closest thing this
 * port has to the flash the section assumes. What a later stage changes is
 * where the archive comes from, not what it is: the format is the one in
 * sys/bootarch.h either way.
 *
 * BOOT_ARCHIVE_PATH is supplied by sys/Makefile, which builds the archive
 * from the programs under bin/.
 */

.section .rodata
.balign 4096
.global boot_archive
boot_archive:
    .incbin BOOT_ARCHIVE_PATH
.global boot_archive_end
boot_archive_end:
