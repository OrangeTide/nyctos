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
 * QEMU's virt board puts RAM at 0x80000000, and the default is 128 MiB. A
 * machine started with more is using what the constant says; a machine
 * started with less faults on the frame check at boot, which is the point
 * at which somebody finds out rather than the point at which a page table
 * lands in nothing.
 */
#define RAM_BASE    0x80000000UL
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

/*
 * The privilege level this kernel runs at, for the boot log.
 *
 * It is worth a line of output because it is not visible any other way and
 * because getting it wrong is silent until something touches a register
 * the mode does not have. This port moved down a mode for the MMU, and a
 * build that quietly went back would otherwise look identical.
 */
const char *
md_mode(void)
{
    return "supervisor";
}

/*
 * The half of the kernel image that is executed, which a Tier 2 map gives
 * different permissions from the half that is written.
 *
 * They cannot share a mapping. AArch64 refuses to execute at EL1 from any
 * page user mode can write, which is a rule the hardware applies whatever
 * the execute-never bits say, and it is the right rule: a kernel that
 * executes memory a program can edit is not a kernel. RISC-V has no such
 * rule and gets the same split anyway, because one map is easier to reason
 * about than two.
 */
extern char _erodata[];
extern char _kernel_end[];

void
md_kernel_image(uintptr_t *text, uintptr_t *rx_end, uintptr_t *end)
{
    *text = (uintptr_t)_start;
    *rx_end = (uintptr_t)_erodata;
    *end = (uintptr_t)_kernel_end;
}
