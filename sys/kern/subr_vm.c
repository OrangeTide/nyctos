/*
 * SPDX-License-Identifier: 0BSD OR Apache-2.0
 * Copyright (C) 2026 Jon Mayo <jon@rm-f.net>
 */

/*
 * Address spaces: the kernel's, and one per process.
 *
 * Every space contains the same kernel, mapped at the address it is loaded
 * at, and one of them additionally contains a program. Identity for the
 * kernel, because it is linked to run where it is loaded and moving it to
 * a high half is a relink of both ports for a benefit this milestone
 * cannot spend.
 *
 * The kernel is in every space rather than shared between them, which
 * costs a handful of frames per process. Sharing it would mean sharing the
 * tables that describe it, and that is only possible where kernel and user
 * addresses fall in different top-level entries. They do not here: a
 * program is linked inside the same gigabyte the kernel occupies, because
 * at Tier 0 that address is physical memory and there is only one of it.
 *
 * What every space leaves out is the window loaded programs live in. In a
 * process space that window holds that process's own memory; in the
 * kernel's it holds nothing at all, because at Tier 2 nothing runs at
 * those addresses except a program in its own space.
 */

#include <sys/page.h>
#include <sys/systm.h>
#include <sys/vm.h>

#if SYS_TIER == 2

static struct addrspace kernel_as;
static uintptr_t window_base;
static size_t window_len;

struct addrspace *
vm_kernel(void)
{
    return &kernel_as;
}

void
vm_user_window(uintptr_t *base, size_t *len)
{
    *base = window_base;
    *len = window_len;
}

/*
 * Everything the kernel needs to reach, in whichever space it is running
 * in: its own image, split where it stops executing and starts writing;
 * the memory frames come from; and the registers it drives directly.
 *
 * AArch64 refuses to execute at EL1 from a page user mode can write, so
 * the split is forced rather than tidy. The writable half is reachable
 * from user mode at this stage only because a program's startup record is
 * still inside it, and stage 4 is where that stops being true.
 */
static sys_err_t
map_kernel_into(struct addrspace *as)
{
    uintptr_t base, hold, text, rx_end, kend, after;
    size_t len, hold_len;
    sys_err_t err;

    md_memory_map(&base, &len, &hold, &hold_len);
    md_kernel_image(&text, &rx_end, &kend);

    /* Below the image: spare memory that frames come from. */
    if (text > base) {
        err = md_map(as, base, base, text - base, VM_READ | VM_WRITE);
        if (err != SYS_OK) {
            return err;
        }
    }

    /* What the kernel executes, which user mode cannot reach. */
    /*
     * Read and execute, and not write. The kernel has no reason to
     * write its own instructions, and mapping them writable meant
     * that something which did went unnoticed until the instructions
     * themselves stopped decoding.
     */
    err = md_map(as, text, text, rx_end - text, VM_READ | VM_EXEC);
    if (err != SYS_OK) {
        return err;
    }

    /* What the kernel writes. */
    err = md_map(as, rx_end, rx_end, kend - rx_end, VM_READ | VM_WRITE);
    if (err != SYS_OK) {
        return err;
    }

    /*
     * Above the image, up to the window programs live in, and then
     * everything above the window. The hole is what a process fills with
     * memory of its own.
     */
    err = md_map(as, kend, kend, window_base - kend, VM_READ | VM_WRITE);
    if (err != SYS_OK) {
        return err;
    }

    after = window_base + window_len;
    err = md_map(as, after, after, base + len - after, VM_READ | VM_WRITE);
    if (err != SYS_OK) {
        return err;
    }

    return md_map_devices(as);
}

sys_err_t
vm_as_create(struct addrspace *as)
{
    sys_err_t err = md_as_create(as);

    if (err != SYS_OK) {
        return err;
    }

    err = map_kernel_into(as);
    if (err != SYS_OK) {
        return err;
    }

    return SYS_OK;
}

void
vm_kernel_init(void)
{
    uintptr_t base, hold;
    size_t len, hold_len;
    uint32_t before = page_used();

    boot_user_window(&window_base, &window_len);
    md_memory_map(&base, &len, &hold, &hold_len);

    if (vm_as_create(&kernel_as) != SYS_OK) {
        panic("vm: could not build the kernel's own address space");
    }

    md_mmu_enable(&kernel_as);

    cnprintf("vm: translation on, %zu MiB mapped, %u frames of tables\n",
      len / (1024 * 1024), page_used() - before);
    cnprintf("  programs live in %p..%p, which no other space maps\n",
      (void *)window_base, (void *)(window_base + window_len));
}

#endif /* SYS_TIER == 2 */
