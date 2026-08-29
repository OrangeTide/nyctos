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

#include <sys/cpu.h>
#include <sys/page.h>
#include <sys/systm.h>
#include <sys/thread.h>
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


/*
 * What the buffer path costs, per section 18's question.
 *
 * The numerator is counted in the ports, because what matters is what
 * the hardware description actually had written to it and not what the
 * caller asked for: a port that maps a block where another maps a page
 * does less work, and a count taken above them could not tell. The
 * denominator is counted in the rendezvous, because how many pages were
 * asked for and how many times is a question about the message and not
 * about the tables.
 *
 * They are charged only while a thread is inside the move. Loading a
 * program writes an entry per page of its slot, two hundred and
 * fifty-six per process, and a total that included those would be a
 * number about the loader with the buffer path lost in the noise.
 *
 * Charged to the thread and not to a global flag. The first version
 * used a global and said in this comment that nothing else could run
 * between the begin and the end, because neither half of the move can
 * block. Blocking is not the only way something else runs: the timer
 * interrupt calls sched_undefer() on its way out, which dispatches if
 * something better became runnable, so a thread can be preempted in the
 * middle of the move and whatever ran next would have been charged to
 * it. Per thread, the preempting thread is not charging and its work is
 * its own.
 *
 * A depth rather than a flag on the thread as well, because the failure
 * paths of the move undo it by calling the other half.
 */
/*
 * One set per core, added up when the report is read.
 *
 * Every event these count happens on a core: a table is written by the
 * core walking it, a translation is thrown away by the core holding
 * it, and a switch is a core switching. So they are per core by nature
 * and were one set for the machine only because there was one core.
 *
 * Two cores incrementing one shared word lose one of the increments
 * whenever the two land together, and these numbers are in the part of
 * the report the check compares exactly. A number that is usually
 * right is worse than no number: it makes the check fail sometimes,
 * which is the failure that gets a check deleted.
 */
static struct vm_cost vm_buffer_cost[SYS_MAX_CORES];

#define vm_cost_self  (vm_buffer_cost[cpu_self()->cp_index])

static bool
charging(void)
{
    const struct thread *t = thread_current();

    return t != NULL && t->t_charging != 0;
}

void
vm_charge_begin(void)
{
    thread_current()->t_charging++;
}

void
vm_charge_end(void)
{
    struct thread *t = thread_current();

    if (t->t_charging == 0) {
        panic("vm: %s was charged out of the buffer path more than in",
          t->t_name);
    }
    t->t_charging--;
}

void
vm_charge_leaf(void)
{
    if (charging()) {
        vm_cost_self.vc_leaf++;
    }
}

void
vm_charge_table(void)
{
    if (charging()) {
        vm_cost_self.vc_table++;
    }
}

void
vm_charge_flush(void)
{
    if (charging()) {
        vm_cost_self.vc_flush++;
    }
}

void
vm_charge_inval(void)
{
    if (charging()) {
        vm_cost_self.vc_inval++;
    }
}

/*
 * Not charged and not bracketed: a switch happens between threads and
 * belongs to neither. It is counted always because it is the thing the
 * buffer path has to be compared against.
 */
void
vm_count_switch(void)
{
    vm_cost_self.vc_switch++;
}

/*
 * What was asked for, which is the other half of the rate. A page is
 * one page moved one way; a half is one hand-over or one take-back.
 * They have different denominators and that is the point: an entry is
 * written per page, and the translation cache is thrown away once per
 * half however many pages moved.
 */
void
vm_charge_pages(uint32_t pages)
{
    vm_cost_self.vc_pages += pages;
    vm_cost_self.vc_halves++;
}

void
vm_cost_read(struct vm_cost *out)
{
    uint32_t i;

    out->vc_leaf = 0;
    out->vc_table = 0;
    out->vc_flush = 0;
    out->vc_inval = 0;
    out->vc_pages = 0;
    out->vc_halves = 0;
    out->vc_switch = 0;

    for (i = 0; i < SYS_MAX_CORES; i++) {
        out->vc_leaf += vm_buffer_cost[i].vc_leaf;
        out->vc_table += vm_buffer_cost[i].vc_table;
        out->vc_flush += vm_buffer_cost[i].vc_flush;
        out->vc_inval += vm_buffer_cost[i].vc_inval;
        out->vc_pages += vm_buffer_cost[i].vc_pages;
        out->vc_halves += vm_buffer_cost[i].vc_halves;
        out->vc_switch += vm_buffer_cost[i].vc_switch;
    }
}

void
vm_cost_read_core(struct vm_cost *out, uint32_t core)
{
    if (core >= SYS_MAX_CORES) {
        return;
    }

    *out = vm_buffer_cost[core];
}
