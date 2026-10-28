/* nycsym.c : name the function each address falls in, from an ELF's symbols.
 *
 * SPDX-License-Identifier: 0BSD OR Apache-2.0
 * Copyright (C) 2026 Jon Mayo <jon@rm-f.net>
 *
 * Reads an ELF and a list of addresses and prints, for each, the function
 * that contains it and the offset into that function. It is the offline
 * half of panic symbolization: a panic prints a record of return addresses,
 * and this names them against the kernel ELF, which keeps its full symbol
 * table on the host even though the kernel-embedded archive does not. Usage:
 *
 *   nycsym kernel.elf 0xADDR [0xADDR ...]
 *
 * Shares sys/elf.h with the kernel so the two read the format one way.
 *
 */

#include <sys/elf.h>

#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void
die(const char *what, const char *detail)
{
    fprintf(stderr, "nycsym: %s: %s\n", what, detail);
    exit(1);
}

static uint8_t *
read_file(const char *path, long *len)
{
    FILE *f = fopen(path, "rb");
    uint8_t *buf;
    long n;

    if (f == NULL)
        die(path, strerror(errno));
    if (fseek(f, 0, SEEK_END) != 0 || (n = ftell(f)) < 0)
        die(path, "cannot size");
    rewind(f);
    buf = malloc((size_t)n);
    if (buf == NULL)
        die(path, "out of memory");
    if (fread(buf, 1, (size_t)n, f) != (size_t)n)
        die(path, "short read");
    fclose(f);
    *len = n;
    return buf;
}

int
main(int argc, char **argv)
{
    const uint8_t *elf;
    const Elf64_Ehdr *eh;
    const Elf64_Sym *syms = NULL;
    const char *strs = NULL;
    uint64_t nsyms = 0;
    long len;
    unsigned int s;
    int i;

    if (argc < 3) {
        fprintf(stderr, "usage: %s elf 0xADDR [0xADDR ...]\n", argv[0]);
        return 1;
    }

    elf = read_file(argv[1], &len);
    eh = (const Elf64_Ehdr *)elf;
    if (eh->e_ident[EI_MAG0] != ELFMAG0 || eh->e_ident[1] != ELFMAG1 ||
        eh->e_ident[2] != ELFMAG2 || eh->e_ident[3] != ELFMAG3)
        die(argv[1], "not an ELF file");

    for (s = 0; s < eh->e_shnum; s++) {
        const Elf64_Shdr *sh = (const Elf64_Shdr *)
          (elf + eh->e_shoff + (uint64_t)s * eh->e_shentsize);

        if (sh->sh_type == SHT_SYMTAB && sh->sh_entsize != 0) {
            const Elf64_Shdr *str = (const Elf64_Shdr *)
              (elf + eh->e_shoff + (uint64_t)sh->sh_link * eh->e_shentsize);

            syms = (const Elf64_Sym *)(elf + sh->sh_offset);
            nsyms = sh->sh_size / sh->sh_entsize;
            strs = (const char *)(elf + str->sh_offset);
            break;
        }
    }
    if (syms == NULL)
        die(argv[1], "carries no symbol table");

    for (i = 2; i < argc; i++) {
        uint64_t addr = strtoull(argv[i], NULL, 0);
        const char *best = NULL;
        uint64_t best_val = 0;
        uint64_t k;

        for (k = 0; k < nsyms; k++) {
            if (ELF64_ST_TYPE(syms[k].st_info) != STT_FUNC)
                continue;
            if (syms[k].st_value > addr)
                continue;
            if (best == NULL || syms[k].st_value > best_val) {
                best = strs + syms[k].st_name;
                best_val = syms[k].st_value;
            }
        }

        if (best == NULL)
            printf("0x%llx ??\n", (unsigned long long)addr);
        else
            printf("0x%llx %s+0x%llx\n", (unsigned long long)addr, best,
              (unsigned long long)(addr - best_val));
    }

    return 0;
}
