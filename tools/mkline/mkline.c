/* mkline.c : build the kernel's embedded source-line table.
 *
 * SPDX-License-Identifier: 0BSD OR Apache-2.0
 * Copyright (C) 2026 Jon Mayo <jon@rm-f.net>
 *
 * Reads the kernel's first-pass ELF, runs its DWARF line program to the full
 * list of (address, file, line) rows, and writes them as the delta-encoded
 * blob the second-pass link embeds so a running kernel can turn a text
 * address into a source line. Usage:
 *
 *   mkline kernel.elf out.blob
 *   mkline --empty out.blob
 *
 * It reads DWARF version 4 and version 5 line programs, the two the targets
 * emit. Shares sys/elf.h and sys/kline.h with the kernel, so the writer and
 * the reader agree on the ELF read and the blob written.
 *
 */

#include <sys/elf.h>
#include <sys/kline.h>

#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

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
#define MAX_FILES   4096

static void
die(const char *what, const char *detail)
{
    fprintf(stderr, "mkline: %s: %s\n", what, detail);
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

static const char *
basename_of(const char *path)
{
    const char *b = path;
    const char *s;

    if (path == NULL)
        return "";
    for (s = path; *s != '\0'; s++) {
        if (*s == '/')
            b = s + 1;
    }
    return b;
}

/* One collected row: a text address, the source line there, and the index of
 * the file in the global file list. */
struct row {
    uint64_t addr;
    uint32_t line;
    uint32_t file;
};

static struct row *rows;
static size_t nrows, caprows;

/* The global file list, base names deduplicated across units. */
static char *files[MAX_FILES];
static int nfiles;

static int
file_index(const char *path)
{
    const char *b = basename_of(path);
    int i;

    for (i = 0; i < nfiles; i++) {
        if (strcmp(files[i], b) == 0)
            return i;
    }
    if (nfiles == MAX_FILES)
        return 0;
    files[nfiles] = strdup(b);
    if (files[nfiles] == NULL)
        die("files", "out of memory");
    return nfiles++;
}

static void
add_row(uint64_t addr, uint32_t line, int file)
{
    if (nrows == caprows) {
        caprows = caprows ? caprows * 2 : 4096;
        rows = realloc(rows, caprows * sizeof(*rows));
        if (rows == NULL)
            die("rows", "out of memory");
    }
    rows[nrows].addr = addr;
    rows[nrows].line = line;
    rows[nrows].file = (uint32_t)file;
    nrows++;
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

/* Run every DWARF line unit and collect its rows. A file index in a row is
 * into the global list, so rows from different units share one numbering. */
static void
collect(const uint8_t *ln, long ln_n, const char *line_str, long line_str_n,
    const char *str, long str_n)
{
    struct cur unit;

    unit.p = ln;
    unit.end = ln + ln_n;
    while (unit.p < unit.end) {
        struct cur c;
        int unit_files[MAX_FILES];
        const uint8_t *uend, *prog;
        uint32_t unit_len, hdr_len;
        uint16_t ver;
        uint8_t min_inst, max_ops, line_range, opcode_base;
        uint8_t std_len[32];
        int8_t line_base;
        int nuf = 0, i;
        uint64_t address, file, line;

        c.p = unit.p;
        c.end = unit.end;
        unit_len = rd_u32(&c);
        if (unit_len == 0 || unit_len >= 0xfffffff0)
            break;
        uend = c.p + unit_len;
        if (uend > unit.end)
            uend = unit.end;
        unit.p = uend;
        c.end = uend;

        ver = rd_u16(&c);
        if (ver != 4 && ver != 5)
            continue;
        if (ver >= 5) {
            (void)rd_u8(&c);    /* address size */
            (void)rd_u8(&c);    /* segment selector size */
        }
        hdr_len = rd_u32(&c);
        prog = c.p + hdr_len;
        min_inst = rd_u8(&c);
        max_ops = rd_u8(&c);
        (void)max_ops;
        (void)rd_u8(&c);        /* default_is_stmt */
        line_base = (int8_t)rd_u8(&c);
        line_range = rd_u8(&c);
        if (line_range == 0)
            line_range = 1;
        opcode_base = rd_u8(&c);
        for (i = 1; i < opcode_base && i < 32; i++)
            std_len[i] = rd_u8(&c);

        if (ver >= 5) {
            uint64_t ctype[MAX_FMT], cform[MAX_FMT];
            uint64_t fmt_n, count, e;
            int keep;

            for (keep = 0; keep <= 1; keep++) {
                fmt_n = rd_u8(&c);
                for (i = 0; (uint64_t)i < fmt_n; i++) {
                    ctype[i < MAX_FMT ? i : 0] = rd_uleb(&c);
                    cform[i < MAX_FMT ? i : 0] = rd_uleb(&c);
                }
                count = rd_uleb(&c);
                for (e = 0; e < count; e++) {
                    const char *path = NULL;

                    for (i = 0; (uint64_t)i < fmt_n; i++) {
                        uint64_t ty = ctype[i < MAX_FMT ? i : 0];
                        uint64_t fm = cform[i < MAX_FMT ? i : 0];
                        const char *s = read_form(&c, fm, line_str,
                          line_str_n, str, str_n);

                        if (ty == DW_LNCT_path)
                            path = s;
                    }
                    if (keep && nuf < MAX_FILES)
                        unit_files[nuf++] = file_index(path);
                }
            }
        } else {
            const char *s;

            do {
                s = read_cstr(&c);
            } while (*s != '\0');
            unit_files[nuf++] = file_index("");     /* index 0 unused */
            for (;;) {
                s = read_cstr(&c);
                if (*s == '\0')
                    break;
                (void)rd_uleb(&c);
                (void)rd_uleb(&c);
                (void)rd_uleb(&c);
                if (nuf < MAX_FILES)
                    unit_files[nuf++] = file_index(s);
            }
        }

        c.p = prog;
        address = 0;
        file = 1;
        line = 1;
        while (c.p < uend) {
            uint8_t op = rd_u8(&c);
            int emit = 0, done = 0;

            if (op == 0) {
                uint64_t len = rd_uleb(&c);
                const uint8_t *next = c.p + len;
                uint8_t sub = rd_u8(&c);

                if (sub == DW_LNE_end_sequence)
                    done = 1;
                else if (sub == DW_LNE_set_address)
                    address = len >= 9 ? rd_u64(&c) : rd_u32(&c);
                c.p = next;
            } else if (op < opcode_base) {
                switch (op) {
                case DW_LNS_copy:
                    emit = 1;
                    break;
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
            } else {
                int adj = op - opcode_base;

                address += min_inst * (adj / line_range);
                line = (uint64_t)((int64_t)line + line_base + adj % line_range);
                emit = 1;
            }

            if (emit) {
                int fi = file < (uint64_t)nuf ? unit_files[file] : 0;

                add_row(address, (uint32_t)line, fi);
            }
            if (done) {
                address = 0;
                file = 1;
                line = 1;
            }
        }
    }
}

static int
by_addr(const void *a, const void *b)
{
    uint64_t x = ((const struct row *)a)->addr;
    uint64_t y = ((const struct row *)b)->addr;

    return x < y ? -1 : x > y ? 1 : 0;
}

static void
put_uleb(FILE *f, uint64_t v)
{
    do {
        uint8_t b = v & 0x7f;

        v >>= 7;
        if (v != 0)
            b |= 0x80;
        fputc(b, f);
    } while (v != 0);
}

static void
put_sleb(FILE *f, int64_t v)
{
    int more = 1;

    while (more) {
        uint8_t b = v & 0x7f;

        v >>= 7;
        if ((v == 0 && !(b & 0x40)) || (v == -1 && (b & 0x40)))
            more = 0;
        else
            b |= 0x80;
        fputc(b, f);
    }
}

static long
uleb_len(uint64_t v)
{
    long n = 0;

    do {
        v >>= 7;
        n++;
    } while (v != 0);
    return n;
}

static long
sleb_len(int64_t v)
{
    long n = 0;
    int more = 1;

    while (more) {
        uint8_t b = v & 0x7f;

        v >>= 7;
        if ((v == 0 && !(b & 0x40)) || (v == -1 && (b & 0x40)))
            more = 0;
        n++;
    }
    return n;
}

int
main(int argc, char **argv)
{
    const uint8_t *elf, *ln;
    const Elf64_Ehdr *eh;
    const char *line_str, *str;
    long len, ln_n = 0, line_str_n = 0, str_n = 0;
    struct kline_hdr hdr;
    uint64_t prev_addr = 0;
    int64_t prev_line = 0, prev_file = 0;
    uint32_t strsize = 0, streamsize = 0;
    size_t i;
    int f;
    FILE *out;

    if (argc != 3) {
        fprintf(stderr, "usage: %s kernel.elf out.blob\n", argv[0]);
        fprintf(stderr, "       %s --empty out.blob\n", argv[0]);
        return 1;
    }

    if (strcmp(argv[1], "--empty") == 0) {
        struct kline_hdr empty = { 0, 0, 0, 0 };

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

    ln = section(elf, eh, ".debug_line", &ln_n);
    line_str = (const char *)section(elf, eh, ".debug_line_str", &line_str_n);
    str = (const char *)section(elf, eh, ".debug_str", &str_n);
    if (ln == NULL)
        die(argv[1], "carries no .debug_line");

    collect(ln, ln_n, line_str, line_str_n, str, str_n);
    qsort(rows, nrows, sizeof(*rows), by_addr);

    /* Drop a row whose address and line repeat the one before it: the reader
     * keeps the last row at or below an address, so a repeat adds nothing. */
    {
        size_t w = 0;

        for (i = 0; i < nrows; i++) {
            if (w > 0 && rows[i].addr == rows[w - 1].addr) {
                rows[w - 1] = rows[i];  /* same address, let the later win */
                continue;
            }
            rows[w++] = rows[i];
        }
        nrows = w;
    }

    for (f = 0; f < nfiles; f++)
        strsize += (uint32_t)strlen(files[f]) + 1;

    for (i = 0; i < nrows; i++) {
        streamsize += (uint32_t)uleb_len(rows[i].addr - prev_addr);
        streamsize += (uint32_t)sleb_len((int64_t)rows[i].line - prev_line);
        streamsize += (uint32_t)sleb_len((int64_t)rows[i].file - prev_file);
        prev_addr = rows[i].addr;
        prev_line = rows[i].line;
        prev_file = rows[i].file;
    }

    out = fopen(argv[2], "wb");
    if (out == NULL)
        die(argv[2], strerror(errno));

    hdr.count = (uint32_t)nrows;
    hdr.filecount = (uint32_t)nfiles;
    hdr.strsize = strsize;
    hdr.streamsize = streamsize;
    if (fwrite(&hdr, sizeof(hdr), 1, out) != 1)
        die(argv[2], "short write");

    {
        uint32_t off = 0;

        for (f = 0; f < nfiles; f++) {
            if (fwrite(&off, sizeof(off), 1, out) != 1)
                die(argv[2], "short write");
            off += (uint32_t)strlen(files[f]) + 1;
        }
    }
    for (f = 0; f < nfiles; f++) {
        if (fwrite(files[f], 1, strlen(files[f]) + 1, out) !=
            strlen(files[f]) + 1)
            die(argv[2], "short write");
    }

    prev_addr = 0;
    prev_line = 0;
    prev_file = 0;
    for (i = 0; i < nrows; i++) {
        put_uleb(out, rows[i].addr - prev_addr);
        put_sleb(out, (int64_t)rows[i].line - prev_line);
        put_sleb(out, (int64_t)rows[i].file - prev_file);
        prev_addr = rows[i].addr;
        prev_line = rows[i].line;
        prev_file = rows[i].file;
    }
    if (fclose(out) != 0)
        die(argv[2], "close failed");

    printf("mkline: %u rows, %u files, %u bytes of rows\n",
      (unsigned)nrows, (unsigned)nfiles, (unsigned)streamsize);
    return 0;
}
