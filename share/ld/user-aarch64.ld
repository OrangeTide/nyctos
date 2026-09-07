/*
 * SPDX-License-Identifier: 0BSD OR Apache-2.0
 * Copyright (C) 2026 Jon Mayo <jon@rm-f.net>
 */

/*
 * Userland images are linked at a fixed address well clear of the kernel,
 * which loads them there verbatim. There is no MMU at Tier 0, so two
 * programs live at once need distinct addresses: the base is
 * USER_LOAD_ADDR from share/mk/nyc.own.mk and each program adds a multiple
 * of USER_LOAD_STRIDE, passed to the linker by share/mk/nyc.prog.mk.
 * Section 15.2 is where a loader starts placing images instead.
 */
ENTRY(_start)

SECTIONS
{
    . = USER_LOAD_ADDR;

    .text : {
        KEEP(*(.text.start))
        *(.text*)
    }

    .rodata : {
        *(.rodata*)
    }

    /*
     * Everything writable starts on a page boundary, so that no page
     * of this image is both text and data. Section 15.1 has writable
     * memory copied eagerly and read-only memory paged in on demand,
     * and a page holding some of each cannot be either: the loader
     * would leave it out and the first write to it would fault
     * against a page a pager is only allowed to supply read-only.
     *
     * It costs padding at the end of the image, up to a page per
     * program, in exchange for a boundary the loader can see.
     *
     * M14 is where the loader sees it. _rw_start is this boundary
     * named, so that the build can put it in the boot archive and the
     * loader can map what is below it read-only and executable and
     * what is above it writable and not.
     */
    . = ALIGN(4096);
    _rw_start = .;

    .data : {
        *(.data*)
    }

    /* Not part of the flat binary: crt1 zeroes it at startup. */
    .bss (NOLOAD) : {
        _bss_start = .;
        *(.bss*)
        *(COMMON)
        _bss_end = .;
    }

    . = ALIGN(16);
    . = . + 0x4000; /* 16 KiB user stack */
    _stack_top = .;

    /*
     * The end of what this program needs, named for the boot archive
     * and rounded to a page because that is the unit the loader maps
     * in. Everything between _rw_start and here is writable: bss,
     * which crt1 zeroes, and the stack below this address.
     */
    . = ALIGN(4096);
    _mem_end = .;

    /DISCARD/ : { *(.comment) *(.note*) }
}
