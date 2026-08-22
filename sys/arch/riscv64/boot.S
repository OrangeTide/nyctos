/*
 * SPDX-License-Identifier: 0BSD OR Apache-2.0
 * Copyright (C) 2026 Jon Mayo <jon@rm-f.net>
 */

/*
 * Entry, and the one thing this port does in machine mode.
 *
 * The kernel runs in supervisor mode, because that is the only mode on
 * this architecture that has an MMU: satp does nothing in machine mode,
 * where the only protection is the PMP. Milestone 4 needs page tables, so
 * the kernel had to move down a mode, and this is the whole of what stays
 * behind: hand the traps, the timer and the memory to supervisor mode, and
 * leave.
 *
 * There is no firmware under this. A port that boots through SBI gets all
 * of the below from the firmware instead and starts at supervisor_start,
 * which is why the two are separated here rather than run together.
 */

.section .text.boot
.global _start

_start:
    /* Zero the .bss section before entering C code. */
    la      t0, _bss_start
    la      t1, _bss_end
zero_bss:
    bge     t0, t1, zero_done
    sd      zero, 0(t0)
    addi    t0, t0, 8
    j       zero_bss
zero_done:

    la      sp, _stack_top

    /*
     * Somewhere for a machine-mode trap to land. Nothing here expects
     * one: everything delegatable is delegated below, no machine
     * interrupt is ever enabled, and the kernel never returns to this
     * mode. What it catches is this window. An instruction the part
     * does not have, and menvcfg below is the candidate, traps to
     * mtvec, and an mtvec nobody set is address zero: the machine
     * stops with nothing printed, which is how this was found.
     */
    la      t0, machine_fatal
    csrw    mtvec, t0

    /*
     * The invariant the trap path reads: sscratch holds a parked
     * kernel stack pointer while a thread is in user mode and zero
     * while it is in the kernel. Zero here rather than trusting the
     * reset value, because a warm restart or a firmware that used it
     * would otherwise send the first kernel trap to a stack made of
     * whatever was left behind.
     */
    csrw    sscratch, zero

    /*
     * Everything the hardware will let supervisor mode have. Writing all
     * ones is deliberate: the bits that cannot be delegated, which are the
     * machine-mode interrupts and the machine-mode environment call, are
     * read-only zero, so the register ends up holding exactly what this
     * hardware permits rather than what this code guessed it permits.
     */
    li      t0, -1
    csrw    medeleg, t0
    csrw    mideleg, t0

    /*
     * The PMP covers all of memory, permitting everything. It is not a
     * protection mechanism here and never was: supervisor and user mode
     * are both subject to it, so without an entry supervisor mode could
     * reach nothing at all. Section 6 puts protection in the MMU, and this
     * is what gets out of its way.
     */
    li      t0, -1
    srli    t0, t0, 10              /* NAPOT range covering all of memory */
    csrw    pmpaddr0, t0
    li      t0, 0x1f                /* NAPOT, read/write/execute */
    csrw    pmpcfg0, t0

    /*
     * The counters supervisor mode may read, which is how it tells the
     * time: rdtime is a counter read rather than a device access, and
     * without this it traps as an illegal instruction.
     */
    li      t0, -1
    csrw    mcounteren, t0

    /*
     * Sstc: the supervisor timer compare register. With it, supervisor
     * mode programs its own timer and machine mode is not involved in a
     * timer interrupt at all. Without it, machine mode would have to take
     * every timer interrupt and forward it, which is a machine-mode trap
     * handler this port would otherwise not need. QEMU's virt board has
     * it; a part without it needs that handler, and the difference is
     * confined to these four instructions and md_timer_at.
     */
    li      t0, 1
    slli    t0, t0, 63              /* menvcfg.STCE */
    csrs    menvcfg, t0

    la      t0, trap_entry
    csrw    stvec, t0

    /*
     * Down to supervisor mode. mret is the only instruction that changes
     * privilege downward, so entering supervisor mode means returning to
     * it from a trap that never happened.
     */
    li      t0, (3 << 11)
    csrc    mstatus, t0             /* clear MPP */
    li      t0, (1 << 11)
    csrs    mstatus, t0             /* MPP = supervisor */

    la      t0, supervisor_start
    csrw    mepc, t0
    mret

supervisor_start:
    call    kernel_main

hang:
    wfi
    j       hang

/*
 * A machine-mode trap, which is a trap nothing expected.
 *
 * It cannot use the kernel's panic path: that runs in supervisor mode and
 * this is not it. What it can do is the one thing section 27 promises is
 * always available, which is the polled UART, and then stop. The message
 * is written a byte at a time from here because a formatter is more than
 * this is entitled to assume.
 */
.balign 4
machine_fatal:
    la      t0, machine_fatal_msg
1:
    lbu     t1, 0(t0)
    beqz    t1, 2f
    li      t2, 0x10000000          /* the 16550's transmit register */
3:
    lbu     t3, 5(t2)               /* line status */
    andi    t3, t3, 0x20            /* transmit holding register empty */
    beqz    t3, 3b
    sb      t1, 0(t2)
    addi    t0, t0, 1
    j       1b
2:
    wfi
    j       2b

.section .rodata
machine_fatal_msg:
    .asciz "\r\npanic: a machine-mode trap in the boot window; see boot.S\r\n"
