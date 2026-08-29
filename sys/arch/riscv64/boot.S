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

#include <sys/config.h>

#define SECONDARY_STACK_BYTES   4096

/* The core-local interruptor. Its first words are one per hart. */
#define CLINT_MSIP              0x02000000

.section .text.boot
.global _start

_start:
    /*
     * Which hart this is. Every one of them arrives here, because
     * there is no firmware under this kernel to hold the others back,
     * so the first thing any of them does is find out whether it is
     * the one that boots.
     *
     * Hart zero by identity rather than by arrival: the parking loop
     * below writes a word the boot hart reads, and a race over who
     * zeroes the .bss is not a race worth having.
     */
    csrr    s0, mhartid
    bnez    s0, secondary_park

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
     *
     * One machine interrupt is enabled now and is handled rather than
     * fatal. See machine_trap.
     *
     * The save area is set up first. That handler borrows registers
     * through mscratch, so pointing mtvec at it while mscratch is
     * still whatever reset left is a window in which a machine trap
     * writes to that address, traps again, and loops with nothing
     * printed, which is the exact failure the paragraph above says
     * this code exists to prevent.
     */
    call    machine_ipi_setup

    la      t0, machine_trap
    csrw    mtvec, t0

    li      t0, 8                   /* mie.MSIE, once mtvec is valid */
    csrs    mie, t0

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
 * Every hart that is not the boot hart, waiting to be wanted.
 *
 * It says it is here and then spins, in machine mode, touching nothing:
 * the .bss it would write is being zeroed by the other hart and the
 * kernel it would enter does not exist yet. Saying it is here is the
 * only way this port can count cores, because without firmware there is
 * nothing to ask.
 */
secondary_park:
    /*
     * A hart the build has no room for parks here and touches nothing.
     * The words below are indexed by hart number, and a hart numbered
     * past the end of them would write its presence into the array
     * that follows: hart three saying it is here is hart one being
     * released, before there is an address for it to be released to.
     */
    li      t0, SYS_MAX_CORES
    bgeu    s0, t0, hart_unusable

    la      t0, hart_present
    slli    t1, s0, 3
    add     t0, t0, t1
    li      t2, 1
    sd      t2, 0(t0)
    fence   w, w

    la      t0, hart_release
    add     t0, t0, t1
1:
    ld      t2, 0(t0)
    beqz    t2, 1b
    fence   r, r

    /*
     * Its own stack, from the array the linker reserved, and then the
     * same handover to supervisor mode the boot hart made. Everything
     * between here and the mret is per hart: the delegation registers,
     * the PMP and the trap vector are all machine-mode state and every
     * hart has its own.
     */
    la      sp, secondary_stacks
    addi    t1, s0, 1
    li      t2, SECONDARY_STACK_BYTES
    mul     t1, t1, t2
    add     sp, sp, t1

    call    machine_ipi_setup

    la      t0, machine_trap
    csrw    mtvec, t0

    li      t0, 8                   /* mie.MSIE, once mtvec is valid */
    csrs    mie, t0

    csrw    sscratch, zero

    li      t0, -1
    csrw    medeleg, t0
    csrw    mideleg, t0

    li      t0, -1
    srli    t0, t0, 10
    csrw    pmpaddr0, t0
    li      t0, 0x1f
    csrw    pmpcfg0, t0

    li      t0, -1
    csrw    mcounteren, t0

    li      t0, 1
    slli    t0, t0, 63
    csrs    menvcfg, t0

    la      t0, trap_entry
    csrw    stvec, t0

    li      t0, (3 << 11)
    csrc    mstatus, t0
    li      t0, (1 << 11)
    csrs    mstatus, t0

    la      t0, supervisor_secondary
    csrw    mepc, t0
    mret

hart_unusable:
    wfi
    j       hart_unusable

supervisor_secondary:
    /*
     * Its identity, in the register it is about to keep its own state
     * in. Supervisor mode cannot read mhartid, so the one chance to
     * pass this is on the way down, and the kernel reads it once and
     * then owns the register.
     */
    mv      tp, s0

    la      t0, hart_entry
    ld      t0, 0(t0)
    jr      t0

/*
 * There is no firmware under this kernel, so the job firmware would do
 * for an inter-processor interrupt is done here.
 *
 * A hart is poked by writing its word in the CLINT, and that word
 * raises a machine software interrupt, which is one of the interrupts
 * that cannot be delegated: the machine mode bits in mideleg are
 * read-only zero however hard the line above writes ones into it. So
 * the poke lands here, in a mode the kernel left and does not return
 * to, and what this does is hand it to the mode the kernel is in.
 *
 * That is exactly what an SBI implementation does with the same
 * interrupt. This kernel is its own.
 */
.balign 4
machine_trap:
    /*
     * Somewhere to put three registers without disturbing the mode this
     * interrupted. mscratch holds this hart's own area, which is what
     * makes this safe with two harts in here at once.
     */
    csrrw   sp, mscratch, sp
    sd      t0, 0(sp)
    sd      t1, 8(sp)
    sd      t2, 16(sp)

    csrr    t0, mcause
    li      t1, 3                   /* machine software interrupt */
    li      t2, 1
    slli    t2, t2, 63              /* the interrupt bit */
    or      t1, t1, t2
    bne     t0, t1, machine_trap_fatal

    /*
     * Taken back before it is passed on. The CLINT word stays raised
     * until it is written, and a hart that returns without clearing it
     * arrives back here immediately and forever.
     */
    csrr    t0, mhartid
    slli    t0, t0, 2
    li      t1, CLINT_MSIP
    add     t1, t1, t0
    sw      zero, 0(t1)

    /* And raised for supervisor mode, which is where the kernel is. */
    li      t1, 2                   /* mip.SSIP */
    csrs    mip, t1

    ld      t0, 0(sp)
    ld      t1, 8(sp)
    ld      t2, 16(sp)
    csrrw   sp, mscratch, sp
    mret

machine_trap_fatal:
    j       machine_fatal

/*
 * The save area this hart's machine-mode trap borrows registers
 * through. Set before mtvec points at that handler and before the
 * interrupt is enabled, in that order, because the handler is useless
 * without it and worse than useless with a bad one.
 */
.balign 4
machine_ipi_setup:
    csrr    t0, mhartid
    slli    t0, t0, 5               /* thirty-two bytes each */
    la      t1, mtrap_save
    add     t1, t1, t0
    csrw    mscratch, t1
    ret

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

/*
 * The words the harts and the kernel say things to each other with.
 *
 * In .data and not .bss, which is the whole of what makes this work.
 * Every hart arrives at once, so a parked hart writes its presence at
 * the same moment the boot hart is zeroing the .bss, and a word in
 * there is a word that gets erased by the core it was written for. The
 * image carries these already zeroed and nothing rewrites them.
 */
.section .data
.balign 16
.global hart_present
hart_present:
    .space  8 * SYS_MAX_CORES
.global hart_release
hart_release:
    .space  8 * SYS_MAX_CORES
.global hart_entry
hart_entry:
    .space  8

/* The stacks the started ones run on, which nothing touches until then. */
.section .bss
.balign 16
secondary_stacks:
    .space  SECONDARY_STACK_BYTES * SYS_MAX_CORES

/*
 * Where each hart puts three registers while it is in machine mode
 * handing an inter-processor interrupt to supervisor mode. One area per
 * hart, because two of them can be in there at once.
 */
.balign 16
mtrap_save:
    .space  32 * SYS_MAX_CORES
