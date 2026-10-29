/* nycsym.c : name an address, as a function and a source line, from an ELF.
 *
 * SPDX-License-Identifier: 0BSD OR Apache-2.0
 * Copyright (C) 2026 Jon Mayo <jon@rm-f.net>
 *
 * Reads an ELF and a list of addresses and prints, for each, the function
 * that contains it and the offset into that function, and the source file
 * and line when the ELF carries DWARF line records. It is the offline half
 * of panic symbolization: a panic prints a record of return addresses, and
 * this names them against the kernel ELF, which keeps its full symbol table
 * and its DWARF on the host even though the kernel-embedded archive does
 * not. Usage:
 *
 *   nycsym kernel.elf 0xADDR [0xADDR ...]
 *
 * The function name comes from the symbol table, the nearest STT_FUNC at or
 * below the address. The file and line come from the .debug_line program,
 * a DWARF version 5 line-number state machine; an address outside every
 * line record, or an ELF without the section, prints the function alone.
 * Shares sys/elf.h with the kernel so the two read the container one way.
 *
 */

#include <sys/elf.h>

#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* DWARF line-number program opcodes and the forms its header uses. */
#define DW_LNS_copy                 1
#define DW_LNS_advance_pc           2
#define DW_LNS_advance_line         3
#define DW_LNS_set_file             4
#define DW_LNS_set_column           5
#define DW_LNS_negate_stmt          6
#define DW_LNS_set_basic_block      7
#define DW_LNS_const_add_pc         8
#define DW_LNS_fixed_advance_pc     9
#define DW_LNS_set_prologue_end     10
#define DW_LNS_set_epilogue_begin   11
#define DW_LNS_set_isa              12

#define DW_LNE_end_sequence         1
#define DW_LNE_set_address          2
#define DW_LNE_set_discriminator    4

#define DW_LNCT_path                1

#define DW_FORM_block2              0x03
#define DW_FORM_block4              0x04
#define DW_FORM_data2               0x05
#define DW_FORM_data4               0x06
#define DW_FORM_data8               0x07
#define DW_FORM_string              0x08
#define DW_FORM_block               0x09
#define DW_FORM_block1              0x0a
#define DW_FORM_data1               0x0b
#define DW_FORM_flag                0x0c
#define DW_FORM_sdata               0x0d
#define DW_FORM_strp                0x0e
#define DW_FORM_udata               0x0f
#define DW_FORM_sec_offset          0x17
#define DW_FORM_data16              0x1e
#define DW_FORM_line_strp           0x1f
#define DW_FORM_strx                0x1a
#define DW_FORM_strx1               0x25
#define DW_FORM_strx2               0x26
#define DW_FORM_strx3               0x27
#define DW_FORM_strx4               0x28

#define MAX_FMT     8
#define MAX_FILES   1024

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

/* A bounded cursor over a byte span, so a malformed record stops rather than
 * walks off the end. Every read clamps to the end and advances no further. */
struct cur {
    const uint8_t *p;
    const uint8_t *end;
};

static uint8_t
rd_u8(struct cur *c)
{
    if (c->p >= c->end)
        return 0;
    return *c->p++;
}

static uint16_t
rd_u16(struct cur *c)
{
    uint16_t v = (uint16_t)rd_u8(c);

    v |= (uint16_t)rd_u8(c) << 8;
    return v;
}

static uint32_t
rd_u32(struct cur *c)
{
    uint32_t v = rd_u16(c);

    v |= (uint32_t)rd_u16(c) << 16;
    return v;
}

static uint64_t
rd_u64(struct cur *c)
{
    uint64_t v = rd_u32(c);

    v |= (uint64_t)rd_u32(c) << 32;
    return v;
}

static uint64_t
rd_uleb(struct cur *c)
{
    uint64_t v = 0;
    int shift = 0;
    uint8_t b;

    do {
        b = rd_u8(c);
        if (shift < 64)
            v |= (uint64_t)(b & 0x7f) << shift;
        shift += 7;
    } while (b & 0x80);

    return v;
}

static int64_t
rd_sleb(struct cur *c)
{
    int64_t v = 0;
    int shift = 0;
    uint8_t b;

    do {
        b = rd_u8(c);
        if (shift < 64)
            v |= (int64_t)(b & 0x7f) << shift;
        shift += 7;
    } while (b & 0x80);
    if (shift < 64 && (b & 0x40))
        v |= -((int64_t)1 << shift);

    return v;
}

/* Read a path-valued form, or skip any other form. Returns the string for
 * the forms a line header uses for a path, and NULL otherwise, having in
 * both cases stepped the cursor past the value. */
static const char *
read_form(struct cur *c, uint64_t form, const char *line_str, long line_str_n,
    const char *str, long str_n)
{
    uint64_t off, n;

    switch (form) {
    case DW_FORM_string: {
        const char *s = (const char *)c->p;

        while (c->p < c->end && *c->p != '\0')
            c->p++;
        if (c->p < c->end)
            c->p++;
        return s;
    }
    case DW_FORM_line_strp:
        off = rd_u32(c);
        return off < (uint64_t)line_str_n ? line_str + off : NULL;
    case DW_FORM_strp:
        off = rd_u32(c);
        return off < (uint64_t)str_n ? str + off : NULL;
    case DW_FORM_sec_offset:
    case DW_FORM_data4:
        c->p += 4;
        return NULL;
    case DW_FORM_data1:
    case DW_FORM_flag:
    case DW_FORM_strx1:
        c->p += 1;
        return NULL;
    case DW_FORM_data2:
    case DW_FORM_strx2:
        c->p += 2;
        return NULL;
    case DW_FORM_strx3:
        c->p += 3;
        return NULL;
    case DW_FORM_data8:
    case DW_FORM_strx4:
        c->p += 8;
        return NULL;
    case DW_FORM_data16:
        c->p += 16;
        return NULL;
    case DW_FORM_udata:
    case DW_FORM_strx:
        (void)rd_uleb(c);
        return NULL;
    case DW_FORM_sdata:
        (void)rd_sleb(c);
        return NULL;
    case DW_FORM_block1:
        n = rd_u8(c);
        c->p += n;
        return NULL;
    case DW_FORM_block2:
        n = rd_u16(c);
        c->p += n;
        return NULL;
    case DW_FORM_block4:
        n = rd_u32(c);
        c->p += n;
        return NULL;
    case DW_FORM_block:
        n = rd_uleb(c);
        c->p += n;
        return NULL;
    default:
        return NULL;
    }
}

/*
 * Read a DWARF 5 directory or file-name table. Each entry carries the fields
 * named by a small format descriptor; the path field is kept into files[],
 * the rest skipped. Directories are read only to step past them.
 */
static void
read_table(struct cur *c, const char **files, int *nfiles, int keep,
    const char *line_str, long line_str_n, const char *str, long str_n)
{
    uint64_t ctype[MAX_FMT], cform[MAX_FMT];
    uint64_t fmt_n, count, i, e;

    fmt_n = rd_u8(c);
    for (i = 0; i < fmt_n; i++) {
        ctype[i < MAX_FMT ? i : 0] = rd_uleb(c);
        cform[i < MAX_FMT ? i : 0] = rd_uleb(c);
    }
    count = rd_uleb(c);
    for (e = 0; e < count; e++) {
        const char *path = NULL;

        for (i = 0; i < fmt_n; i++) {
            uint64_t ty = ctype[i < MAX_FMT ? i : 0];
            uint64_t fm = cform[i < MAX_FMT ? i : 0];
            const char *s = read_form(c, fm, line_str, line_str_n, str, str_n);

            if (ty == DW_LNCT_path)
                path = s;
        }
        if (keep && *nfiles < MAX_FILES)
            files[(*nfiles)++] = path;
    }
}

/*
 * Read a DWARF 4 directory and file-name table. The directories are a run of
 * strings ended by an empty one; the files follow the same way, each with a
 * directory index, time and length after its name. The files are numbered
 * from one, so index zero is left unused to match the file register.
 */
static const char *
read_cstr(struct cur *c)
{
    const char *s = (const char *)c->p;

    while (c->p < c->end && *c->p != '\0')
        c->p++;
    if (c->p < c->end)
        c->p++;
    return s;
}

static void
read_v4_tables(struct cur *c, const char **files, int *nfiles)
{
    const char *s;

    do {
        s = read_cstr(c);
    } while (*s != '\0');

    files[0] = NULL;
    *nfiles = 1;
    for (;;) {
        s = read_cstr(c);
        if (*s == '\0')
            break;
        (void)rd_uleb(c);       /* directory index */
        (void)rd_uleb(c);       /* modification time */
        (void)rd_uleb(c);       /* length */
        if (*nfiles < MAX_FILES)
            files[(*nfiles)++] = s;
    }
}

static const char *
basename_of(const char *path)
{
    const char *b = path;
    const char *s;

    if (path == NULL)
        return NULL;
    for (s = path; *s != '\0'; s++) {
        if (*s == '/')
            b = s + 1;
    }
    return b;
}

/*
 * Find the source file and line for an address by running the .debug_line
 * program. A row holds an address, and the row's file and line describe the
 * span from that address to the next row's; the span that contains the
 * target is the answer. Returns 1 and fills out_file and out_line on a hit.
 */
static int
lookup_line(const uint8_t *ln, long ln_n, const char *line_str, long line_str_n,
    const char *str, long str_n, uint64_t target,
    const char **out_file, int *out_line)
{
    struct cur unit;

    unit.p = ln;
    unit.end = ln + ln_n;
    while (unit.p < unit.end) {
        struct cur c;
        const char *files[MAX_FILES];
        const uint8_t *uend, *prog;
        uint32_t unit_len, hdr_len;
        uint16_t ver;
        uint8_t min_inst, max_ops, line_range, opcode_base;
        uint8_t std_len[32];
        int8_t line_base;
        int nfiles = 0, i;
        uint64_t address, file, line;
        int have_prev = 0;
        uint64_t prev_addr = 0, prev_file = 0, prev_line = 0;

        c.p = unit.p;
        c.end = unit.end;
        unit_len = rd_u32(&c);
        if (unit_len == 0 || unit_len >= 0xfffffff0)
            break;              /* empty, or 64-bit DWARF this does not read */
        uend = c.p + unit_len;
        if (uend > unit.end)
            uend = unit.end;
        unit.p = uend;          /* next unit, whatever this one holds */
        c.end = uend;

        ver = rd_u16(&c);
        if (ver != 4 && ver != 5)
            continue;           /* this reader is DWARF 4 or 5 */
        if (ver >= 5) {
            (void)rd_u8(&c);    /* address size */
            (void)rd_u8(&c);    /* segment selector size */
        }
        hdr_len = rd_u32(&c);
        prog = c.p + hdr_len;
        min_inst = rd_u8(&c);
        max_ops = rd_u8(&c);
        if (max_ops == 0)
            max_ops = 1;
        (void)rd_u8(&c);        /* default_is_stmt */
        line_base = (int8_t)rd_u8(&c);
        line_range = rd_u8(&c);
        if (line_range == 0)
            line_range = 1;
        opcode_base = rd_u8(&c);
        for (i = 1; i < opcode_base && i < 32; i++)
            std_len[i] = rd_u8(&c);
        if (ver >= 5) {
            read_table(&c, files, &nfiles, 0, line_str, line_str_n, str,
              str_n);
            read_table(&c, files, &nfiles, 1, line_str, line_str_n, str,
              str_n);
        } else {
            read_v4_tables(&c, files, &nfiles);
        }

        c.p = prog;
        address = 0;
        file = 1;
        line = 1;
        while (c.p < uend) {
            uint8_t op = rd_u8(&c);

            if (op == 0) {
                uint64_t len = rd_uleb(&c);
                const uint8_t *next = c.p + len;
                uint8_t sub = rd_u8(&c);

                if (sub == DW_LNE_end_sequence) {
                    if (have_prev && prev_addr <= target &&
                        target < address) {
                        *out_file = prev_file < (uint64_t)nfiles
                          ? files[prev_file] : NULL;
                        *out_line = (int)prev_line;
                        return 1;
                    }
                    address = 0;
                    file = 1;
                    line = 1;
                    have_prev = 0;
                } else if (sub == DW_LNE_set_address) {
                    address = len >= 9 ? rd_u64(&c) : rd_u32(&c);
                }
                c.p = next;
            } else if (op < opcode_base) {
                switch (op) {
                case DW_LNS_copy:
                    goto row;
                case DW_LNS_advance_pc:
                    address += min_inst * rd_uleb(&c);
                    break;
                case DW_LNS_advance_line:
                    line = (uint64_t)((int64_t)line + rd_sleb(&c));
                    break;
                case DW_LNS_set_file:
                    file = rd_uleb(&c);
                    break;
                case DW_LNS_set_column:
                    (void)rd_uleb(&c);
                    break;
                case DW_LNS_negate_stmt:
                case DW_LNS_set_basic_block:
                case DW_LNS_set_prologue_end:
                case DW_LNS_set_epilogue_begin:
                    break;
                case DW_LNS_const_add_pc:
                    address += min_inst * ((255 - opcode_base) / line_range);
                    break;
                case DW_LNS_fixed_advance_pc:
                    address += rd_u16(&c);
                    break;
                case DW_LNS_set_isa:
                    (void)rd_uleb(&c);
                    break;
                default: {
                    int k;

                    for (k = 0; op < 32 && k < std_len[op]; k++)
                        (void)rd_uleb(&c);
                    break;
                }
                }
                continue;
            } else {
                int adj = op - opcode_base;

                address += min_inst * (adj / line_range);
                line = (uint64_t)((int64_t)line + line_base + adj % line_range);
                goto row;
            }
            continue;
    row:
            if (have_prev && prev_addr <= target && target < address) {
                *out_file = prev_file < (uint64_t)nfiles
                  ? files[prev_file] : NULL;
                *out_line = (int)prev_line;
                return 1;
            }
            prev_addr = address;
            prev_file = file;
            prev_line = line;
            have_prev = 1;
        }
    }

    return 0;
}

static const uint8_t *
section(const uint8_t *elf, const Elf64_Ehdr *eh, const char *name, long *size)
{
    const Elf64_Shdr *shstr = (const Elf64_Shdr *)
      (elf + eh->e_shoff + (uint64_t)eh->e_shstrndx * eh->e_shentsize);
    const char *names = (const char *)(elf + shstr->sh_offset);
    unsigned int s;

    for (s = 0; s < eh->e_shnum; s++) {
        const Elf64_Shdr *sh = (const Elf64_Shdr *)
          (elf + eh->e_shoff + (uint64_t)s * eh->e_shentsize);

        if (strcmp(names + sh->sh_name, name) == 0) {
            *size = (long)sh->sh_size;
            return elf + sh->sh_offset;
        }
    }
    *size = 0;
    return NULL;
}

int
main(int argc, char **argv)
{
    const uint8_t *elf;
    const Elf64_Ehdr *eh;
    const Elf64_Sym *syms = NULL;
    const char *strs = NULL;
    const uint8_t *ln;
    const char *line_str, *dstr;
    long ln_n = 0, line_str_n = 0, dstr_n = 0;
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

    ln = section(elf, eh, ".debug_line", &ln_n);
    line_str = (const char *)section(elf, eh, ".debug_line_str", &line_str_n);
    dstr = (const char *)section(elf, eh, ".debug_str", &dstr_n);

    for (i = 2; i < argc; i++) {
        uint64_t addr = strtoull(argv[i], NULL, 0);
        const char *best = NULL;
        const char *file = NULL;
        uint64_t best_val = 0;
        uint64_t k;
        int line = 0;

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
            printf("0x%llx ??", (unsigned long long)addr);
        else
            printf("0x%llx %s+0x%llx", (unsigned long long)addr, best,
              (unsigned long long)(addr - best_val));
        if (ln != NULL && lookup_line(ln, ln_n, line_str, line_str_n, dstr,
            dstr_n, addr, &file, &line) && file != NULL)
            printf(" %s:%d", basename_of(file), line);
        printf("\n");
    }

    return 0;
}
