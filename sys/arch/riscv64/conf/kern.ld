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
        /*
         * The small sections too. RISC-V gives short-reach addressing
         * its own sections, and a script that names only the ordinary
         * ones leaves them as orphans the linker places wherever it
         * likes. Read-only ones belong with rodata; the writable
         * ones belong below, and .sbss belongs inside the range
         * whatever zeroes bss walks. It was outside it.
         */
        *(.srodata*)
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
        *(.sdata*)
    }

    .bss (NOLOAD) : {
        _bss_start = .;
        *(.sbss*)
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
