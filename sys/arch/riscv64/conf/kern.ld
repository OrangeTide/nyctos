/*
 * SPDX-License-Identifier: 0BSD OR Apache-2.0
 * Copyright (C) 2026 Jon Mayo <jon@rm-f.net>
 */

/*
 * QEMU 'virt' riscv64 machine with -bios none: the reset vector jumps
 * straight to 0x80000000, so that is where our kernel must be linked.
 * Userland is loaded at 0x80200000, well above anything the kernel image
 * occupies.
 */
ENTRY(_start)

SECTIONS
{
    . = 0x80000000;

    .text : {
        KEEP(*(.text.boot))
        *(.text*)
    }

    .rodata : {
        *(.rodata*)
    }

    .data : {
        *(.data*)
    }

    .bss (NOLOAD) : {
        _bss_start = .;
        *(.bss*)
        *(COMMON)
        _bss_end = .;
    }

    . = ALIGN(16);
    . = . + 0x4000; /* 16 KiB boot stack */
    _stack_top = .;

    /DISCARD/ : { *(.comment) *(.note*) }
}
