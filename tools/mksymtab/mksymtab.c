/* mksymtab.c : build the kernel's embedded function-symbol table.
 *
 * SPDX-License-Identifier: 0BSD OR Apache-2.0
 * Copyright (C) 2026 Jon Mayo <jon@rm-f.net>
 *
 * Reads the kernel's first-pass ELF and writes a blob of its function
 * symbols, sorted by address, for the second-pass link to embed so the
 * running kernel can name an address. Usage:
 *
 *   mksymtab kernel.elf out.blob
 *
 * Shares sys/elf.h and sys/ksym.h with the kernel, so the writer and the
 * reader agree on both the ELF format read and the blob format written.
 *
 */

#include <sys/elf.h>
#include <sys/ksym.h>

#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void
die(const char *what, const char *detail)
{
    fprintf(stderr, "mksymtab: %s: %s\n", what, detail);
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

struct built {
    uint64_t addr;
    uint32_t size;
    uint32_t name;      /* offset into our string table */
};

static int
by_addr(const void *a, const void *b)
{
    uint64_t x = ((const struct built *)a)->addr;
    uint64_t y = ((const struct built *)b)->addr;

    return x < y ? -1 : x > y ? 1 : 0;
}

int
main(int argc, char **argv)
{
    const uint8_t *elf;
    const Elf64_Ehdr *eh;
    const Elf64_Sym *syms = NULL;
    const char *strs = NULL;
    struct built *ents;
    char *strtab;
    struct ksym_hdr hdr;
    uint64_t nsyms = 0, k;
    uint32_t count = 0, strsize = 1;    /* a leading NUL, so name 0 is "" */
    long len;
    unsigned int s;
    FILE *out;

    if (argc != 3) {
        fprintf(stderr, "usage: %s kernel.elf out.blob\n", argv[0]);
        fprintf(stderr, "       %s --empty out.blob\n", argv[0]);
        return 1;
    }

    /*
     * The stub the first pass embeds: a header saying no functions, so the
     * blob symbols are defined and in range for a PC-relative reference
     * before the real table exists.
     */
    if (strcmp(argv[1], "--empty") == 0) {
        struct ksym_hdr empty = { 0, 0 };

        out = fopen(argv[2], "wb");
        if (out == NULL)
            die(argv[2], strerror(errno));
        if (fwrite(&empty, sizeof(empty), 1, out) != 1)
            die(argv[2], "short write");
        if (fclose(out) != 0)
            die(argv[2], "close failed");
        return 0;
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

    ents = calloc(nsyms, sizeof(*ents));
    strtab = malloc(1);
    if (ents == NULL || strtab == NULL)
        die("table", "out of memory");
    strtab[0] = '\0';

    for (k = 0; k < nsyms; k++) {
        const char *name;
        size_t nlen;

        if (ELF64_ST_TYPE(syms[k].st_info) != STT_FUNC)
            continue;
        name = strs + syms[k].st_name;
        if (name[0] == '\0')
            continue;

        nlen = strlen(name) + 1;
        strtab = realloc(strtab, strsize + nlen);
        if (strtab == NULL)
            die("strings", "out of memory");
        memcpy(strtab + strsize, name, nlen);

        ents[count].addr = syms[k].st_value;
        ents[count].size = (uint32_t)syms[k].st_size;
        ents[count].name = strsize;
        count++;
        strsize += (uint32_t)nlen;
    }

    qsort(ents, count, sizeof(*ents), by_addr);

    out = fopen(argv[2], "wb");
    if (out == NULL)
        die(argv[2], strerror(errno));

    hdr.count = count;
    hdr.strsize = strsize;
    if (fwrite(&hdr, sizeof(hdr), 1, out) != 1)
        die(argv[2], "short write");
    for (k = 0; k < count; k++) {
        struct ksym_ent e;

        e.addr = ents[k].addr;
        e.size = ents[k].size;
        e.name = ents[k].name;
        if (fwrite(&e, sizeof(e), 1, out) != 1)
            die(argv[2], "short write");
    }
    if (fwrite(strtab, 1, strsize, out) != strsize)
        die(argv[2], "short write");
    if (fclose(out) != 0)
        die(argv[2], "close failed");

    printf("mksymtab: %u functions, %u bytes of names\n", count, strsize);
    return 0;
}
