/*
 * SPDX-License-Identifier: 0BSD OR Apache-2.0
 * Copyright (C) 2026 Jon Mayo <jon@rm-f.net>
 */

/*
 * QEMU 'virt' aarch64 machine: RAM starts at 0x40000000, and the -kernel
 * loader places ELF images at RAM base + 0x80000 by default. Userland is
 * loaded at 0x40200000, well above anything the kernel image occupies.
 */
ENTRY(_start)

SECTIONS
{
    . = 0x40080000;

    .text : {
        KEEP(*(.text.boot))
        *(.text*)
    }

    .rodata : {
        *(.rodata*)
    }

    /*
     * The end of what the kernel executes, page-aligned so that a Tier
     * 2 map can give it different permissions from the writable half.
     * AArch64 will not execute at EL1 from a page user mode can write,
     * so the two halves cannot share a mapping.
     */
    . = ALIGN(4096);
    _erodata = .;

    .data : {
        *(.data*)
    }

    .bss (NOLOAD) : {
        _bss_start = .;
        *(.bss*)
        *(COMMON)
        _bss_end = .;
    }

    . = ALIGN(4096);
    _stack_bottom = .;
    . = . + 0x4000; /* 16 KiB boot stack */
    _stack_top = .;
    . = ALIGN(4096);
    _kernel_end = .;

    /DISCARD/ : { *(.comment) *(.note*) }
}
