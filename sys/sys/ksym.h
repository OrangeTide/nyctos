/* ksym.h : the kernel's embedded function-symbol table.
 *
 * SPDX-License-Identifier: 0BSD OR Apache-2.0
 * Copyright (C) 2026 Jon Mayo <jon@rm-f.net>
 *
 * The format of the symbol blob the mksymtab host tool builds from the
 * kernel's first-pass ELF and the second-pass link embeds, and the lookup
 * the kernel does against it. A header, then one entry per function sorted
 * by address, then the names the entries point into. Included by the kernel
 * and by mksymtab so the writer and the reader agree on one format.
 *
 */

#ifndef _SYS_KSYM_H_
#define _SYS_KSYM_H_

#include <stdint.h>

struct ksym_hdr {
    uint32_t    count;          /* function entries that follow */
    uint32_t    strsize;        /* bytes of the string table after them */
};

struct ksym_ent {
    uint64_t    addr;           /* the function's virtual address */
    uint32_t    size;           /* its byte length, zero if unknown */
    uint32_t    name;           /* offset into the string table */
};

/*
 * The name of the function a kernel virtual address falls in, and through
 * off the distance into it, or NULL when no table is embedded or no
 * function precedes the address. The function is the one with the greatest
 * address at or below it.
 */
const char *ksym_lookup(uintptr_t addr, uintptr_t *off);

#endif /* _SYS_KSYM_H_ */
