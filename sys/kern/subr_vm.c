/*
 * SPDX-License-Identifier: 0BSD OR Apache-2.0
 * Copyright (C) 2026 Jon Mayo <jon@rm-f.net>
 */

/*
 * The kernel's own address space.
 *
 * What it contains is everything the kernel touches, mapped at the address
 * it already had: the memory the port handed over, and the registers the
 * kernel reaches directly. Identity, because the kernel is linked to run
 * where it is loaded and moving it to a high half is a relink of both
 * ports for no benefit this milestone can spend.
 *
 * Nothing is protected yet, and that is this stage rather than an
 * oversight. The whole of memory is mapped so that user mode can reach it,
 * because a program's startup record still lives in a kernel structure and
 * its stack still lives inside the image it was loaded from. Stage 4 is
 * where a process gets a space of its own and those two facts stop being
 * true; until then translation is on and enforcement is not, which is a
 * state worth passing through deliberately: it separates "the tables are
 * right" from "the permissions are right", and each is a different way to
 * be wrong.
 */

#include <sys/page.h>
#include <sys/systm.h>
#include <sys/vm.h>

#if SYS_TIER == 2

static struct addrspace kernel_as;

struct addrspace *
vm_kernel(void)
{
    return &kernel_as;
}

void
vm_kernel_init(void)
{
    uintptr_t base, hold, text, rx_end, kend;
    uint32_t before;
    size_t len, hold_len;

    before = page_used();

    if (md_as_create(&kernel_as) != SYS_OK) {
        panic("vm: no frame for the kernel's root table");
    }

    md_memory_map(&base, &len, &hold, &hold_len);

    md_kernel_image(&text, &rx_end, &kend);

    /*
     * Four ranges, and the shape of them is forced rather than
     * chosen. AArch64 refuses to execute at EL1 from a page user
     * mode can write, so what the kernel executes and what a
     * program can reach cannot be the same mapping. The first
     * attempt at this stage mapped all of memory as everything to
     * everyone, and the machine faulted on the instruction after
     * the one that enabled translation, for exactly that reason.
     *
     * Below the image, spare memory that frames come from. The kernel
     * reads and writes it and nobody executes it.
     */
    if (text > base && md_map(&kernel_as, base, base, text - base,
      VM_READ | VM_WRITE) != SYS_OK) {
        panic("vm: could not map the memory below the kernel");
    }

    /* What the kernel executes, which user mode cannot reach. */
    if (md_map(&kernel_as, text, text, rx_end - text,
      VM_READ | VM_WRITE | VM_EXEC) != SYS_OK) {
        panic("vm: could not map the kernel's text");
    }

    /*
     * What the kernel writes, which user mode can read because the
     * startup record of section 20.4 is still inside it. That is
     * the deviation this stage carries and stage 4 removes: the
     * record belongs in the child's own memory and lives in a
     * kernel structure until a child has memory of its own.
     */
    if (md_map(&kernel_as, rx_end, rx_end, kend - rx_end,
      VM_READ | VM_WRITE | VM_USER) != SYS_OK) {
        panic("vm: could not map the kernel's data");
    }

    /*
     * Everything above the image, which is where loaded programs
     * are and where frames come from. Executable by user mode
     * because a program's text is in it.
     */
    if (md_map(&kernel_as, kend, kend, base + len - kend,
      VM_READ | VM_WRITE | VM_USER | VM_EXEC) != SYS_OK) {
        panic("vm: could not map memory above the kernel");
    }

    if (md_map_devices(&kernel_as) != SYS_OK) {
        panic("vm: could not map the registers the kernel uses");
    }

    md_as_activate(&kernel_as);

    cnprintf("vm: translation on, %zu MiB mapped, %u frames of tables\n",
      len / (1024 * 1024), page_used() - before);
}

#endif /* SYS_TIER == 2 */
