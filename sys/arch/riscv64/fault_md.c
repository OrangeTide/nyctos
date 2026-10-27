/* fault_md.c : RISC-V mapping from scause to fault kinds.
 *
 * SPDX-License-Identifier: 0BSD OR Apache-2.0
 * Copyright (C) 2026 Jon Mayo <jon@rm-f.net>
 *
 * Maps each synchronous scause exception code to a fault kind. Codes that
 * cannot arrive on this system are listed explicitly with a comment
 * giving the reason. md_fault_has_addr reports which causes carry a
 * faulting address in stval. md_fault_check verifies the table against
 * its expected values, and md_fault_access reports whether the faulting
 * access was a fetch, a load, or a store.
 *
 */

#include <sys/op.h>
#include <sys/systm.h>

#define CAUSE_INSN_MISALIGNED   0
#define CAUSE_INSN_ACCESS       1
#define CAUSE_ILLEGAL_INSN      2
#define CAUSE_BREAKPOINT        3
#define CAUSE_LOAD_MISALIGNED   4
#define CAUSE_LOAD_ACCESS       5
#define CAUSE_STORE_MISALIGNED  6
#define CAUSE_STORE_ACCESS      7
#define CAUSE_ECALL_U           8
#define CAUSE_ECALL_S           9
#define CAUSE_ECALL_M           11
#define CAUSE_INSN_PAGE         12
#define CAUSE_LOAD_PAGE         13
#define CAUSE_STORE_PAGE        15
#define CAUSE_SW_CHECK          18
#define CAUSE_HW_ERROR          19

uint32_t
md_fault_kind(unsigned long cause)
{
    switch (cause) {
    case CAUSE_INSN_MISALIGNED:
    case CAUSE_LOAD_MISALIGNED:
    case CAUSE_STORE_MISALIGNED:
        /*
         * The kernel and its programs are built with strict alignment,
         * so this arrives from a program that constructed an address
         * rather than from a compiler that assumed a fixup.
         */
        return SYS_FAULT_ALIGN;

    case CAUSE_INSN_ACCESS:
    case CAUSE_LOAD_ACCESS:
    case CAUSE_STORE_ACCESS:
        /*
         * A physical access that did not complete: no memory at the
         * address, or PMP refusing it. The PMP here is wide open, so in
         * this system it means the bus had nothing there.
         */
        return SYS_FAULT_ACCESS;

    case CAUSE_INSN_PAGE:
    case CAUSE_LOAD_PAGE:
    case CAUSE_STORE_PAGE:
        /*
         * Translation said no. Sv39 gives no bit that separates "no
         * entry" from "wrong permission" in scause, so this port
         * cannot tell the two apart and answers with the one a pager
         * can do something about. The pager reads the tables, which is
         * where the difference is written down.
         *
         * A page the kernel has taken away is a permission fault by
         * construction on the other port and is this code here, so
         * the two agree on what happens and not on what it is
         * called. What settles it either way is the kernel asking
         * whether the faulting process has a pager and whether the
         * address is one it could be given.
         */
        return SYS_FAULT_PAGE;

    case CAUSE_ILLEGAL_INSN:
        return SYS_FAULT_INSTRUCTION;

    case CAUSE_BREAKPOINT:
        return SYS_FAULT_DEBUG;

    case CAUSE_ECALL_U:
        /*
         * A system call, which the trap path takes before it gets here.
         * Reaching this means the entry path let one through.
         */
        return SYS_FAULT_INSTRUCTION;

    case CAUSE_ECALL_S:
    case CAUSE_ECALL_M:
        /*
         * The kernel calling a level above itself. There is nothing
         * above it: the machine-mode window closed at boot and no
         * hypervisor is present, so this cannot arrive from a program
         * and cannot arrive from the kernel either.
         */
        return SYS_FAULT_INSTRUCTION;

    case CAUSE_SW_CHECK:
    case CAUSE_HW_ERROR:
        /*
         * A landing-pad or shadow-stack check, and a hardware error
         * report. Neither extension is enabled here, and the second is a
         * statement that the memory is untrustworthy rather than that
         * the thread was wrong, which is what SYS_FAULT_EXTERNAL means.
         */
        return cause == CAUSE_HW_ERROR ? SYS_FAULT_EXTERNAL
          : SYS_FAULT_INSTRUCTION;

    default:
        /*
         * A code the architecture reserves, or one added after this was
         * written. Nothing true can be said about it beyond that the
         * instruction did not run.
         *
         * There is no arithmetic row on this port. RISC-V integer
         * division by zero produces a defined value rather than an
         * exception, so SYS_FAULT_ARITH cannot be raised here at all
         * until floating point traps are enabled, and this kernel does
         * not enable them.
         */
        return SYS_FAULT_INSTRUCTION;
    }
}

/*
 * Whether stval holds an address worth reporting. The architecture sets
 * it for the aborts and the misalignments and zeroes it for the rest, so
 * this says the same thing twice; it says it here so that a caller does
 * not have to know which faults those are.
 */
bool
md_fault_has_addr(unsigned long cause)
{
    switch (cause) {
    case CAUSE_INSN_MISALIGNED:
    case CAUSE_LOAD_MISALIGNED:
    case CAUSE_STORE_MISALIGNED:
    case CAUSE_INSN_ACCESS:
    case CAUSE_LOAD_ACCESS:
    case CAUSE_STORE_ACCESS:
    case CAUSE_INSN_PAGE:
    case CAUSE_LOAD_PAGE:
    case CAUSE_STORE_PAGE:
    case CAUSE_BREAKPOINT:
        return true;
    default:
        return false;
    }
}

/*
 * The mapping, checked against the codes named one at a time.
 *
 * The other port groups its abort encodings by masking and lost two
 * groups doing it; this one has no groups and lists every code, so what
 * this catches is a code that moves rather than a group that is missed.
 * The sweep at the end catches the same thing on both: that nothing,
 * defined or reserved, maps to "did not fault".
 */
static void
expect(unsigned long cause, uint32_t want, const char *what)
{
    uint32_t got = md_fault_kind(cause);

    if (got != want) {
        panic("fault map: %s (scause %lu) is %u, expected %u", what, cause,
          (unsigned int)got, (unsigned int)want);
    }
}

void
md_fault_check(void)
{
    unsigned long cause;

    expect(CAUSE_INSN_MISALIGNED, SYS_FAULT_ALIGN, "misaligned fetch");
    expect(CAUSE_LOAD_MISALIGNED, SYS_FAULT_ALIGN, "misaligned load");
    expect(CAUSE_STORE_MISALIGNED, SYS_FAULT_ALIGN, "misaligned store");
    expect(CAUSE_INSN_ACCESS, SYS_FAULT_ACCESS, "fetch access");
    expect(CAUSE_LOAD_ACCESS, SYS_FAULT_ACCESS, "load access");
    expect(CAUSE_STORE_ACCESS, SYS_FAULT_ACCESS, "store access");
    expect(CAUSE_INSN_PAGE, SYS_FAULT_PAGE, "fetch page");
    expect(CAUSE_LOAD_PAGE, SYS_FAULT_PAGE, "load page");
    expect(CAUSE_STORE_PAGE, SYS_FAULT_PAGE, "store page");
    expect(CAUSE_ILLEGAL_INSN, SYS_FAULT_INSTRUCTION, "illegal instruction");
    expect(CAUSE_BREAKPOINT, SYS_FAULT_DEBUG, "breakpoint");
    expect(CAUSE_HW_ERROR, SYS_FAULT_EXTERNAL, "hardware error");
    expect(CAUSE_SW_CHECK, SYS_FAULT_INSTRUCTION, "software check");
    expect(CAUSE_ECALL_S, SYS_FAULT_INSTRUCTION, "ecall from supervisor");

    /*
     * There is no arithmetic row on this port, and this is where that is
     * a statement rather than an omission: no code maps to it, because
     * integer division by zero has a defined result here and floating
     * point traps are not enabled.
     */
    for (cause = 0; cause < 64; cause++) {
        uint32_t kind = md_fault_kind(cause);

        if (kind == SYS_FAULT_NONE) {
            panic("fault map: scause %lu is not a fault", cause);
        }
        if (kind == SYS_FAULT_ARITH) {
            panic("fault map: scause %lu is arithmetic, which this port "
              "cannot raise", cause);
        }
    }
}

/*
 * What the instruction was doing to the address, for a pager deciding
 * what to supply. The cause says it outright on this port, which is one
 * of the places Sv39 is more forthcoming than the other architecture.
 */
uint32_t
md_fault_access(unsigned long cause)
{
    switch (cause) {
    case CAUSE_INSN_PAGE:
    case CAUSE_INSN_ACCESS:
    case CAUSE_INSN_MISALIGNED:
        return SYS_RIGHT_EXEC;
    case CAUSE_STORE_PAGE:
    case CAUSE_STORE_ACCESS:
    case CAUSE_STORE_MISALIGNED:
        return SYS_RIGHT_WRITE;
    default:
        return SYS_RIGHT_READ;
    }
}
