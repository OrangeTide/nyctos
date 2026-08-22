/*
 * SPDX-License-Identifier: 0BSD OR Apache-2.0
 *
 * PUBLIC DOMAIN (CC0-1.0)
 */

/*
 * Every synchronous exception this port can raise, mapped to a kind of
 * section 16.1.
 *
 * The mapping is exhaustive on purpose, including the classes that cannot
 * arrive in this system: a row that says "this cannot happen here" is a
 * statement that survives someone enabling the feature, and a row that is
 * simply absent is a fault that arrives as whatever the default happens
 * to be. Where a class cannot arrive, the comment says why rather than
 * the code omitting it.
 *
 * ESR_EL1 is two pieces. EC, the top six bits, says what class of thing
 * happened. ISS, the bottom twenty-five, says what within that class, and
 * for the two abort classes the bottom six bits of it are the fault
 * status code, which is where the difference between a translation fault,
 * a permission fault and an alignment fault lives.
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
         * Translation fault: no descriptor for the address. This is the
         * row that becomes SYS_FAULT_PAGE when there is a pager to make
         * it recoverable, which is stage 8. Until then nothing can
         * supply the page and calling it recoverable would be a claim
         * about a mechanism that does not exist.
         */
        return SYS_FAULT_ACCESS;
    case 0x08:
        return SYS_FAULT_ACCESS;    /* access flag fault */
    case 0x0c:
        return SYS_FAULT_ACCESS;    /* permission fault */
    case 0x10:
        /*
         * Synchronous external abort, on the access or on the table walk
         * that served it. Section 16.1 keeps this apart from an access
         * fault because it says the memory is untrustworthy rather than
         * that the thread was wrong.
         */
        return SYS_FAULT_EXTERNAL;
    case 0x18:
        return SYS_FAULT_EXTERNAL;  /* parity or ECC error */
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
         * control this kernel did not set. Section 16.1 puts all three
         * in one kind, because what a program can do about any of them
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
