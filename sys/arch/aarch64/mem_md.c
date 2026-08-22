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

/*
 * Which address space the hardware is translating through, for a fault
 * report. A fault whose address is unmapped is a different bug from a
 * fault taken in the wrong space, and the report cannot tell them apart
 * without this.
 */
unsigned long
md_current_space(void)
{
    unsigned long ttbr;

    __asm__ volatile ("mrs %0, ttbr0_el1" : "=r" (ttbr));

    return ttbr;
}

/*
 * The console's registers, for whoever hands them to a driver. The
 * kernel knows the address because it panics through the same device;
 * section 19.2 has a driver told rather than compiled with it, and this
 * is the telling.
 */
void
md_console_regs(uintptr_t *base, size_t *len)
{
    *base = 0x09000000UL;
    *len = 0x1000UL;
}
