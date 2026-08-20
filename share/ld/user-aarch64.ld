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
