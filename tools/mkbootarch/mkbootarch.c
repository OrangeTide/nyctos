/* mkbootarch.c : build-host tool that writes the boot archive image.
 *
 * SPDX-License-Identifier: 0BSD OR Apache-2.0
 * Copyright (C) 2026 Jon Mayo <jon@rm-f.net>
 *
 * Packs a set of program ELFs into one boot archive for the kernel to
 * carry and load at boot. Usage:
 *
 *   mkbootarch out.img name=load@file [name=load@file ...]
 *
 * Each input is a program's ELF. The tool reads the load address, the
 * read-write boundary, the memory extent, and the file extent out of the
 * ELF's PT_LOAD program headers, and stores the ELF itself as the member's
 * payload so a reader downstream has its symbols. It runs on the build host
 * with the host compiler and stdio, and shares only sys/bootarch.h and
 * sys/elf.h with the kernel, so the writer and the reader agree on one
 * description of each format.
 *
 * The command-line load address and a "<file>.layout" beside the ELF are
 * still read and cross-checked against the program headers, so a header the
 * tool reads wrong stops the build rather than loading a program wrong.
 *
 */

#include <sys/bootarch.h>
#include <sys/elf.h>

#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void
die(const char *what, const char *detail)
{
    fprintf(stderr, "mkbootarch: %s: %s\n", what, detail);
    exit(1);
}

/* Round up to the payload granularity. */
static uint32_t
align_up(uint32_t n)
{
    return (n + BOOT_ALIGN - 1u) & ~(BOOT_ALIGN - 1u);
}

static long
file_size(const char *path)
{
    FILE *f = fopen(path, "rb");
    long n;

    if (f == NULL)
        die(path, strerror(errno));
    if (fseek(f, 0, SEEK_END) != 0)
        die(path, strerror(errno));
    n = ftell(f);
    fclose(f);
    if (n < 0)
        die(path, "cannot size");
    return n;
}

static void
copy_into(FILE *out, const char *path)
{
    char buf[65536];
    FILE *f = fopen(path, "rb");
    size_t n;

    if (f == NULL)
        die(path, strerror(errno));
    while ((n = fread(buf, 1, sizeof(buf), f)) > 0) {
        if (fwrite(buf, 1, n, out) != n)
            die(path, "short write");
    }
    fclose(f);
}

static void
pad_to(FILE *out, uint32_t from, uint32_t to)
{
    while (from < to) {
        if (fputc(0, out) == EOF)
            die("output", "short write");
        from++;
    }
}


/*
 * The layout of an image, written beside its binary by the link that
 * produced it: two offsets from the load address, where writable memory
 * begins and where the program's memory ends.
 *
 * Beside the binary rather than on the command line because the numbers
 * come from the linker's symbol table, and a tool that took them as
 * arguments would let the archive rule and the link disagree without
 * anything noticing. Required rather than defaulted: an image whose
 * layout is missing is a build that has half changed, and guessing a
 * whole slot is not something to risk.
 */
static void
read_layout(const char *bin, uint32_t *rw, uint32_t *mem)
{
    char path[4096];
    unsigned long long a, b;
    FILE *f;

    if (snprintf(path, sizeof(path), "%s.layout", bin) >= (int)sizeof(path))
        die(bin, "path too long for its layout");

    f = fopen(path, "r");
    if (f == NULL)
        die(path, strerror(errno));
    if (fscanf(f, "%llx %llx", &a, &b) != 2)
        die(path, "expected two hexadecimal offsets");
    fclose(f);

    if (a == 0 || b <= a)
        die(path, "layout is not ordered: 0 < rw < mem");
    if ((a % BOOT_ALIGN) != 0 || (b % BOOT_ALIGN) != 0)
        die(path, "layout is not aligned to the payload granularity");

    *rw = (uint32_t)a;
    *mem = (uint32_t)b;
}

/*
 * The member's layout, read out of its ELF program headers. bm_load is the
 * lowest loadable virtual address; the read-write boundary is where the
 * first writable segment begins; the memory extent is the highest address
 * any segment occupies in memory; the file extent is the highest address a
 * segment's file bytes reach. All but the load are returned as offsets from
 * it, which is what the archive carries.
 */
static void
parse_elf(const char *path, uint64_t *load, uint32_t *rw, uint32_t *mem,
  uint32_t *size)
{
    FILE *f = fopen(path, "rb");
    Elf64_Ehdr eh;
    uint64_t lo = 0, file_end = 0, mem_end = 0, w_lo = 0;
    int have_load = 0, have_w = 0;
    unsigned int i;

    if (f == NULL)
        die(path, strerror(errno));
    if (fread(&eh, sizeof(eh), 1, f) != 1)
        die(path, "cannot read the ELF header");
    if (eh.e_ident[EI_MAG0] != ELFMAG0 || eh.e_ident[1] != ELFMAG1 ||
        eh.e_ident[2] != ELFMAG2 || eh.e_ident[3] != ELFMAG3)
        die(path, "not an ELF file");
    if (eh.e_ident[EI_CLASS] != ELFCLASS64 ||
        eh.e_ident[EI_DATA] != ELFDATA2LSB)
        die(path, "not a little-endian ELF64");
    if (eh.e_type != ET_EXEC)
        die(path, "not a fixed-address executable");
    if (eh.e_phentsize != sizeof(Elf64_Phdr))
        die(path, "unexpected program header size");

    for (i = 0; i < eh.e_phnum; i++) {
        Elf64_Phdr ph;
        long at = (long)(eh.e_phoff + (uint64_t)i * eh.e_phentsize);

        if (fseek(f, at, SEEK_SET) != 0)
            die(path, "cannot seek to a program header");
        if (fread(&ph, sizeof(ph), 1, f) != 1)
            die(path, "cannot read a program header");
        if (ph.p_type != PT_LOAD)
            continue;

        if (!have_load || ph.p_vaddr < lo)
            lo = ph.p_vaddr;
        have_load = 1;
        if (ph.p_vaddr + ph.p_filesz > file_end)
            file_end = ph.p_vaddr + ph.p_filesz;
        if (ph.p_vaddr + ph.p_memsz > mem_end)
            mem_end = ph.p_vaddr + ph.p_memsz;
        if ((ph.p_flags & PF_W) != 0 && (!have_w || ph.p_vaddr < w_lo)) {
            w_lo = ph.p_vaddr;
            have_w = 1;
        }
    }
    fclose(f);

    if (!have_load)
        die(path, "has no loadable segment");
    if (!have_w)
        die(path, "has no writable segment");

    *load = lo;
    *rw = (uint32_t)(w_lo - lo);
    *mem = (uint32_t)(mem_end - lo);
    *size = (uint32_t)(file_end - lo);
}

/*
 * Split "name=load@file". The load address is where the image is linked to
 * run; the tool does not read it out of the file, because a flat binary
 * carries no such record and inventing one would mean an ELF reader here.
 */
static void
parse(char *spec, char **name, uint64_t *load, char **path)
{
    char *eq = strchr(spec, '=');
    char *at = eq != NULL ? strchr(eq, '@') : NULL;

    if (eq == NULL || at == NULL)
        die(spec, "expected name=load@file");
    *eq = '\0';
    *at = '\0';
    *name = spec;
    *load = strtoull(eq + 1, NULL, 0);
    *path = at + 1;

    if (strlen(*name) >= BOOT_NAME_MAX)
        die(*name, "name does not fit BOOT_NAME_MAX");
    if (*load == 0)
        die(spec, "load address is zero or unparsable");
}

int
main(int argc, char **argv)
{
    struct boot_member *members;
    struct boot_header hdr;
    const char **paths;
    uint32_t count, offset;
    FILE *out;
    int i;

    if (argc < 3) {
        fprintf(stderr, "usage: %s out.img name=load@file ...\n", argv[0]);
        return 1;
    }

    count = (uint32_t)(argc - 2);
    members = calloc(count, sizeof(*members));
    paths = calloc(count, sizeof(*paths));
    if (members == NULL || paths == NULL)
        die("members", "out of memory");

    /*
     * Lay the archive out before writing any of it: every payload offset
     * is known once the record table's size is, and the table's size is
     * known from the count alone.
     *
     * Specs are parsed once, because parsing writes terminators into argv
     * and a second pass over the same string would find nothing.
     */
    offset = align_up((uint32_t)(sizeof(hdr) + count * sizeof(*members)));
    for (i = 0; i < (int)count; i++) {
        char *name, *path;
        uint64_t load, load_e;
        uint32_t rw_e, mem_e, size_e, rw_l, mem_l;
        long elfsize;

        parse(argv[i + 2], &name, &load, &path);
        parse_elf(path, &load_e, &rw_e, &mem_e, &size_e);
        read_layout(path, &rw_l, &mem_l);
        elfsize = file_size(path);
        paths[i] = path;

        /*
         * The program headers against the two older sources, the
         * command-line load address and the layout sidecar. Agreement is
         * the point: a header read wrong stops the build here rather than
         * placing a program at the wrong address or with the wrong bounds.
         */
        if (load_e != load)
            die(name, "its ELF load address is not the one on the command line");
        if (rw_e != rw_l || mem_e != mem_l)
            die(name, "its ELF layout is not the one in the sidecar");

        memset(members[i].bm_name, 0, BOOT_NAME_MAX);
        memcpy(members[i].bm_name, name, strlen(name));
        members[i].bm_load = load_e;
        members[i].bm_offset = offset;
        members[i].bm_size = size_e;
        members[i].bm_rw = rw_e;
        members[i].bm_mem = mem_e;
        members[i].bm_elfsize = (uint32_t)elfsize;

        /*
         * The image bytes have to fit in the memory. They may cross the
         * read-write boundary: above it is the program's initialised data,
         * which is carried in the segment and copied into writable memory,
         * and peer_b is the one image here that has any. What would be
         * wrong is image bytes with nowhere to go.
         */
        if (size_e > mem_e)
            die(name, "its bytes do not fit in the memory it asks for");

        offset = align_up(offset + (uint32_t)elfsize);
    }

    hdr.bh_magic = BOOT_MAGIC;
    hdr.bh_version = BOOT_VERSION;
    hdr.bh_count = count;
    hdr.bh_bytes = offset;

    out = fopen(argv[1], "wb");
    if (out == NULL)
        die(argv[1], strerror(errno));

    if (fwrite(&hdr, sizeof(hdr), 1, out) != 1)
        die(argv[1], "short write");
    if (fwrite(members, sizeof(*members), count, out) != count)
        die(argv[1], "short write");

    offset = (uint32_t)(sizeof(hdr) + count * sizeof(*members));
    for (i = 0; i < (int)count; i++) {
        pad_to(out, offset, members[i].bm_offset);
        copy_into(out, paths[i]);
        offset = members[i].bm_offset + members[i].bm_elfsize;
    }
    pad_to(out, offset, hdr.bh_bytes);

    if (fclose(out) != 0)
        die(argv[1], "close failed");

    printf("mkbootarch: %u members, %u bytes\n", count, hdr.bh_bytes);
    return 0;
}
