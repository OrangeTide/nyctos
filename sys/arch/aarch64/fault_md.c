/* fault_md.c : AArch64 mapping from ESR_EL1 to fault kinds.
 *
 * SPDX-License-Identifier: 0BSD OR Apache-2.0
 * Copyright (C) 2026 Jon Mayo <jon@rm-f.net>
 *
 * Maps each ESR_EL1 exception class to a fault kind. For the abort
 * classes, the fault status code in the low six bits of the ISS selects
 * the kind. Classes that cannot arrive on this system are listed with a
 * comment giving the reason. md_fault_check verifies the table against
 * its expected values, and md_fault_access reports the access an abort
 * was attempting.
 *
 */

#include <sys/op.h>
#include <sys/systm.h>

#define ESR_EC(esr)     (((esr) >> 26) & 0x3fu)
#define ESR_FSC(esr)    ((esr) & 0x3fu)

/* Exception classes, from the architecture's table. */
#define EC_UNKNOWN          0x00
#define EC_WF               0x01    /* trapped WFI or WFE */
#define EC_SIMD_FP          0x07    /* SIMD or floating point, trapped */
#define EC_ILLEGAL_STATE    0x0e
#define EC_SVC64            0x15
#define EC_MSR_TRAP         0x18
#define EC_INSN_ABORT_LOW   0x20
#define EC_INSN_ABORT_CUR   0x21
#define EC_PC_ALIGN         0x22
#define EC_DATA_ABORT_LOW   0x24
#define EC_DATA_ABORT_CUR   0x25
#define EC_SP_ALIGN         0x26
#define EC_FP64             0x2c
#define EC_SERROR           0x2f
#define EC_BREAKPOINT_LOW   0x30
#define EC_BREAKPOINT_CUR   0x31
#define EC_SW_STEP_LOW      0x32
#define EC_SW_STEP_CUR      0x33
#define EC_WATCHPOINT_LOW   0x34
#define EC_WATCHPOINT_CUR   0x35
#define EC_BRK64            0x3c

/*
 * The fault status codes an abort carries, in the groups the
 * architecture defines them in. Only the group matters here: which of
 * the three levels a translation fault happened at is a fact about the
 * page tables and not about the kind of fault.
 */
static uint32_t
abort_kind(unsigned long esr)
{
    uint32_t fsc = (uint32_t)ESR_FSC(esr);

    switch (fsc & 0x3cu) {
    case 0x00:
        /*
         * Address size fault, which is an address with bits set above
         * what the translation regime can describe. Nothing can supply
         * a page for it, so it is an access fault and not a page one.
         */
        return SYS_FAULT_ACCESS;
    case 0x04:
        /*
         * Translation fault: no descriptor for the address. This is
         * the recoverable kind, because a pager can supply what is
         * not there. Whether one exists
         * for the faulting process is the kernel's question and not
         * this port's: what the hardware said is that nothing
         * describes the address.
         *
         * A page that has been taken away rather than never mapped
         * is a permission fault instead, because that is how the
         * kernel takes one away: the entry stays and loses the bit
         * that lets user mode in. The difference matters here, since
         * one of them is a question for a pager and the other is a
         * program touching memory it lent out.
         */
        return SYS_FAULT_PAGE;
    case 0x08:
        return SYS_FAULT_ACCESS;    /* access flag fault */
    case 0x0c:
        return SYS_FAULT_ACCESS;    /* permission fault */
    case 0x10:
        /*
         * Synchronous external abort on the access itself, and, in the
         * same group, the tag check fault of a part with memory tagging.
         * Neither can arrive from an ordinary access here and tagging is
         * not enabled, so grouping them costs nothing and losing the
         * external abort would cost the difference between a thread that
         * was wrong and memory that is not trustworthy.
         */
        return SYS_FAULT_EXTERNAL;
    case 0x14:
        /*
         * The same abort, taken on the translation table walk rather
         * than on the access. It is a separate group in the encoding and
         * was a separate line missing from the first version of this,
         * which reported it as an access fault: a program blamed for a
         * bus that failed under the kernel's own page tables.
         */
        return SYS_FAULT_EXTERNAL;
    case 0x18:
        return SYS_FAULT_EXTERNAL;  /* parity or ECC error */
    case 0x1c:
        return SYS_FAULT_EXTERNAL;  /* the same, on the table walk */
    default:
        break;
    }

    switch (fsc) {
    case 0x21:
        return SYS_FAULT_ALIGN;
    case 0x30:
        return SYS_FAULT_EXTERNAL;  /* TLB conflict */
    default:
        /*
         * Everything left is a lockdown or an unsupported atomic, which
         * this port cannot produce: no lockdown region is programmed and
         * the memory a program reaches is ordinary. It is classed as an
         * access fault rather than as an unknown, because whatever it
         * was, it was a reference to memory that did not complete.
         */
        return SYS_FAULT_ACCESS;
    }
}

uint32_t
md_fault_kind(unsigned long cause)
{
    switch (ESR_EC(cause)) {
    case EC_INSN_ABORT_LOW:
    case EC_INSN_ABORT_CUR:
    case EC_DATA_ABORT_LOW:
    case EC_DATA_ABORT_CUR:
        return abort_kind(cause);

    case EC_PC_ALIGN:
    case EC_SP_ALIGN:
        return SYS_FAULT_ALIGN;

    case EC_FP64:
        /*
         * A floating point exception the hardware was asked to raise.
         * Nothing here enables the traps, and the kernel is built with
         * general registers only, so this arrives from a program that
         * enabled them for itself.
         */
        return SYS_FAULT_ARITH;

    case EC_BREAKPOINT_LOW:
    case EC_BREAKPOINT_CUR:
    case EC_SW_STEP_LOW:
    case EC_SW_STEP_CUR:
    case EC_WATCHPOINT_LOW:
    case EC_WATCHPOINT_CUR:
    case EC_BRK64:
        return SYS_FAULT_DEBUG;

    case EC_SERROR:
        return SYS_FAULT_EXTERNAL;

    case EC_UNKNOWN:
    case EC_WF:
    case EC_SIMD_FP:
    case EC_ILLEGAL_STATE:
    case EC_MSR_TRAP:
        /*
         * An instruction that is undefined, privileged, or trapped by a
         * control this kernel did not set. All three are in one kind,
         * because what a program can do about any of them
         * is the same nothing.
         */
        return SYS_FAULT_INSTRUCTION;

    case EC_SVC64:
        /*
         * A system call, which the trap path takes before it gets here.
         * Reaching this means the entry path let one through, and the
         * thread dying on an instruction fault is the correct outcome
         * of a kernel that cannot service the call.
         */
        return SYS_FAULT_INSTRUCTION;

    default:
        /*
         * A class the architecture defines and this part does not
         * implement, or one added after this was written. There is
         * nothing true to say about it beyond that the instruction
         * could not run.
         */
        return SYS_FAULT_INSTRUCTION;
    }
}

/*
 * Whether the address in the fault report means anything. Only the two
 * abort classes and the two alignment classes set FAR, and reporting a
 * stale one as the address a thread touched is worse than reporting none.
 */
bool
md_fault_has_addr(unsigned long cause)
{
    switch (ESR_EC(cause)) {
    case EC_INSN_ABORT_LOW:
    case EC_INSN_ABORT_CUR:
    case EC_DATA_ABORT_LOW:
    case EC_DATA_ABORT_CUR:
    case EC_PC_ALIGN:
    case EC_WATCHPOINT_LOW:
    case EC_WATCHPOINT_CUR:
        return true;
    default:
        return false;
    }
}

/*
 * The mapping, checked against the encodings named one at a time.
 *
 * This is not the mapping restating itself. Above, the abort classes are
 * grouped by masking off the level bits, which is compact and is exactly
 * where the first version of this went wrong: two of the eight groups
 * were absent, so a synchronous external abort taken on a table walk was
 * reported as a program touching memory it should not have. Here every
 * encoding is written out by name and level, so a group the mask misses
 * is a row that disagrees.
 *
 * The sweep at the end is the weaker half and catches a different thing:
 * that no encoding at all, defined or reserved, maps to "did not fault".
 */
static void
expect(unsigned long esr, uint32_t want, const char *what)
{
    uint32_t got = md_fault_kind(esr);

    if (got != want) {
        panic("fault map: %s (esr 0x%lx) is %u, expected %u", what, esr,
          (unsigned int)got, (unsigned int)want);
    }
}

void
md_fault_check(void)
{
    static const uint32_t aborts[] = {
        EC_INSN_ABORT_LOW, EC_INSN_ABORT_CUR,
        EC_DATA_ABORT_LOW, EC_DATA_ABORT_CUR,
    };
    unsigned int i, level, ec;

    for (i = 0; i < sizeof(aborts) / sizeof(aborts[0]); i++) {
        unsigned long base = (unsigned long)aborts[i] << 26;

        for (level = 0; level < 4; level++) {
            expect(base | (0x00u + level), SYS_FAULT_ACCESS, "address size");
            expect(base | (0x04u + level), SYS_FAULT_PAGE, "translation");
            expect(base | (0x08u + level), SYS_FAULT_ACCESS, "access flag");
            expect(base | (0x0cu + level), SYS_FAULT_ACCESS, "permission");
            expect(base | (0x14u + level), SYS_FAULT_EXTERNAL,
              "external abort on a walk");
            expect(base | (0x1cu + level), SYS_FAULT_EXTERNAL,
              "parity error on a walk");
        }
        expect(base | 0x10u, SYS_FAULT_EXTERNAL, "external abort");
        expect(base | 0x18u, SYS_FAULT_EXTERNAL, "parity error");
        expect(base | 0x21u, SYS_FAULT_ALIGN, "alignment");
        expect(base | 0x30u, SYS_FAULT_EXTERNAL, "TLB conflict");
    }

    expect((unsigned long)EC_PC_ALIGN << 26, SYS_FAULT_ALIGN, "pc alignment");
    expect((unsigned long)EC_SP_ALIGN << 26, SYS_FAULT_ALIGN, "sp alignment");
    expect((unsigned long)EC_FP64 << 26, SYS_FAULT_ARITH, "floating point");
    expect((unsigned long)EC_SERROR << 26, SYS_FAULT_EXTERNAL, "SError");
    expect((unsigned long)EC_BREAKPOINT_LOW << 26, SYS_FAULT_DEBUG,
      "breakpoint");
    expect((unsigned long)EC_SW_STEP_LOW << 26, SYS_FAULT_DEBUG, "step");
    expect((unsigned long)EC_WATCHPOINT_LOW << 26, SYS_FAULT_DEBUG,
      "watchpoint");
    expect((unsigned long)EC_BRK64 << 26, SYS_FAULT_DEBUG, "brk");
    expect((unsigned long)EC_UNKNOWN << 26, SYS_FAULT_INSTRUCTION, "unknown");
    expect((unsigned long)EC_ILLEGAL_STATE << 26, SYS_FAULT_INSTRUCTION,
      "illegal state");

    /* Nothing at all, defined or reserved, says the thread did not fault. */
    for (ec = 0; ec < 64; ec++) {
        unsigned long esr = (unsigned long)ec << 26;
        unsigned int fsc;

        for (fsc = 0; fsc < 64; fsc++) {
            if (md_fault_kind(esr | fsc) == SYS_FAULT_NONE) {
                panic("fault map: esr 0x%lx is not a fault", esr | fsc);
            }
        }
    }
}

/*
 * What the instruction was doing to the address, for a pager deciding
 * what to supply. An instruction abort is a fetch; a data abort says in
 * its ISS whether it was a write.
 */
uint32_t
md_fault_access(unsigned long cause)
{
    switch (ESR_EC(cause)) {
    case EC_INSN_ABORT_LOW:
    case EC_INSN_ABORT_CUR:
        return SYS_RIGHT_EXEC;
    case EC_DATA_ABORT_LOW:
    case EC_DATA_ABORT_CUR:
        return (cause & (1UL << 6)) != 0 ? SYS_RIGHT_WRITE
          : SYS_RIGHT_READ;
    default:
        return SYS_RIGHT_READ;
    }
}
