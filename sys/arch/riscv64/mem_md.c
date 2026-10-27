/* mem_md.c : RISC-V memory layout and console registers.
 *
 * SPDX-License-Identifier: 0BSD OR Apache-2.0
 * Copyright (C) 2026 Jon Mayo <jon@rm-f.net>
 *
 * Reports RAM at 0x80000000 with 128 MiB, and reports the range from
 * _start to _stack_top as held back. The kernel image bounds are _start,
 * _erodata, and _kernel_end. md_console_regs gives the 16550 UART at
 * 0x10000000. md_mode reports supervisor, and md_current_space returns
 * satp.
 *
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

/* Which address space the hardware is translating through; see the other
 * port for why a fault report needs it. */
unsigned long
md_current_space(void)
{
    unsigned long satp;

    __asm__ volatile ("csrr %0, satp" : "=r" (satp));

    return satp;
}

/*
 * The console's registers, for whoever hands them to a driver. The
 * kernel knows the address because it panics through the same device;
 * a driver is told the registers rather than compiled with them, and this
 * is the telling.
 */
void
md_console_regs(uintptr_t *base, size_t *len)
{
    *base = 0x10000000UL;
    *len = 0x1000UL;
}

/*
 * The virtio-mmio transport slots, for whoever hands them to a driver. The
 * 'virt' machine places eight slots of 0x1000 bytes end to end at
 * 0x10001000, and a block device plugged into one lands in a slot the driver
 * finds by scanning. The whole region is granted so the driver can scan it.
 */
void
md_virtio_regs(uintptr_t *base, size_t *len)
{
    *base = 0x10001000UL;
    *len = 8UL * 0x1000UL;
}
