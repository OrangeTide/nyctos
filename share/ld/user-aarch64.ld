/*
 * SPDX-License-Identifier: 0BSD OR Apache-2.0
 * Copyright (C) 2026 Jon Mayo <jon@rm-f.net>
 */

/*
 * Userland images are linked at a fixed address well clear of the kernel,
 * which loads them there verbatim. The address must match USER_LOAD_ADDR
 * in share/mk/nyc.own.mk.
 */
ENTRY(_start)

SECTIONS
{
    . = 0x40200000;

    .text : {
        KEEP(*(.text.start))
        *(.text*)
    }

    .rodata : {
        *(.rodata*)
    }

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

    /DISCARD/ : { *(.comment) *(.note*) }
}
