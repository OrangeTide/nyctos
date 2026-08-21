/*
 * SPDX-License-Identifier: 0BSD OR Apache-2.0
 * Copyright (C) 2026 Jon Mayo <jon@rm-f.net>
 */

/*
 * What this port tells the kernel about the machine it is on.
 */

#include <sys/systm.h>

#include <stddef.h>
#include <stdint.h>

/*
 * QEMU's virt board puts RAM at 0x40000000, and the default is 128 MiB. A
 * machine started with more is using what the constant says; a machine
 * started with less faults on the frame check at boot, which is the point
 * at which somebody finds out rather than the point at which a page table
 * lands in nothing.
 */
#define RAM_BASE    0x40000000UL
#define RAM_BYTES   (128UL * 1024 * 1024)

extern char _start[];
extern char _stack_top[];

void
md_memory_map(uintptr_t *base, size_t *len, uintptr_t *hold, size_t *hold_len)
{
    *base = RAM_BASE;
    *len = RAM_BYTES;
    *hold = (uintptr_t)_start;
    *hold_len = (size_t)((uintptr_t)_stack_top - (uintptr_t)_start);
}

/* The privilege level this kernel runs at, for the boot log. */
const char *
md_mode(void)
{
    return "EL1";
}
