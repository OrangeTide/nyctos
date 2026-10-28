/* elf.h : the ELF64 structures and constants the loader and the archive
 * tool read.
 *
 * SPDX-License-Identifier: 0BSD OR Apache-2.0
 * Copyright (C) 2026 Jon Mayo <jon@rm-f.net>
 *
 * A minimal subset of the ELF64 format: the file header, the program
 * header, and the constants needed to find and place loadable segments.
 * Defined here rather than taken from a host <elf.h> so the kernel and the
 * build-host mkbootarch tool share one description and the tool builds on a
 * host without a system ELF header.
 *
 */

#ifndef _SYS_ELF_H_
#define _SYS_ELF_H_

#include <stdint.h>

typedef struct {
    unsigned char   e_ident[16];
    uint16_t        e_type;
    uint16_t        e_machine;
    uint32_t        e_version;
    uint64_t        e_entry;
    uint64_t        e_phoff;
    uint64_t        e_shoff;
    uint32_t        e_flags;
    uint16_t        e_ehsize;
    uint16_t        e_phentsize;
    uint16_t        e_phnum;
    uint16_t        e_shentsize;
    uint16_t        e_shnum;
    uint16_t        e_shstrndx;
} Elf64_Ehdr;

typedef struct {
    uint32_t        p_type;
    uint32_t        p_flags;
    uint64_t        p_offset;
    uint64_t        p_vaddr;
    uint64_t        p_paddr;
    uint64_t        p_filesz;
    uint64_t        p_memsz;
    uint64_t        p_align;
} Elf64_Phdr;

typedef struct {
    uint32_t        sh_name;
    uint32_t        sh_type;
    uint64_t        sh_flags;
    uint64_t        sh_addr;
    uint64_t        sh_offset;
    uint64_t        sh_size;
    uint32_t        sh_link;
    uint32_t        sh_info;
    uint64_t        sh_addralign;
    uint64_t        sh_entsize;
} Elf64_Shdr;

typedef struct {
    uint32_t        st_name;
    uint8_t         st_info;
    uint8_t         st_other;
    uint16_t        st_shndx;
    uint64_t        st_value;
    uint64_t        st_size;
} Elf64_Sym;

/* e_ident indices and the magic the first four hold. */
#define EI_MAG0         0
#define EI_CLASS        4
#define EI_DATA         5
#define ELFMAG0         0x7f
#define ELFMAG1         'E'
#define ELFMAG2         'L'
#define ELFMAG3         'F'
#define ELFCLASS64      2
#define ELFDATA2LSB     1       /* little-endian, which both ports are */

#define ET_EXEC         2       /* a fixed-address executable */

#define PT_LOAD         1       /* a segment the loader places in memory */

#define PF_X            0x1     /* executable */
#define PF_W            0x2     /* writable */
#define PF_R            0x4     /* readable */

#define SHT_SYMTAB      2       /* a symbol table */
#define SHT_STRTAB      3       /* a string table */

#define STT_FUNC        2       /* the symbol names a function */
#define ELF64_ST_TYPE(i)        ((i) & 0xf)

#endif /* _SYS_ELF_H_ */
