/*
 * SPDX-License-Identifier: 0BSD OR Apache-2.0
 * Copyright (C) 2026 Jon Mayo <jon@rm-f.net>
 */

/*
 * mkfsimg: builds a TFS initramfs image out of ordinary host files.
 *
 * This is a host tool, not part of the kernel or userland: it runs on the
 * machine doing the build, using the host's own libc, and produces the
 * image the kernel embeds and parses at boot (see sys/sys/tfs.h and
 * sys/kern/ramfs.c). The pattern is the same one makefs, genromfs and
 * QNX's mkifs follow: a small offline tool assembles the boot filesystem
 * once, so the on-target driver only ever has to read it.
 *
 * Usage:
 *   mkfsimg -o image.img name=path [name=path ...]
 *
 * Each "name=path" names the file as it will be looked up in the image
 * (e.g. "bin/motd") and the host file supplying its contents.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "sys/tfs.h"

struct entry {
    const char *name;
    const char *path;
    long        size;
    uint32_t    offset;
};

static void
usage(const char *prog)
{
    fprintf(stderr, "usage: %s -o image.img name=path [name=path ...]\n", prog);
    exit(1);
}

static long
file_size(FILE *fp)
{
    long size;

    if (fseek(fp, 0, SEEK_END) != 0) {
        return -1;
    }
    size = ftell(fp);
    if (size < 0) {
        return -1;
    }
    rewind(fp);
    return size;
}

int
main(int argc, char **argv)
{
    const char *out_path = NULL;
    struct entry *entries;
    int nentries = 0;
    int i;
    FILE *out;
    struct tfs_header hdr;
    uint32_t offset;

    if (argc < 4) {
        usage(argv[0]);
    }
    if (strcmp(argv[1], "-o") != 0) {
        usage(argv[0]);
    }
    out_path = argv[2];

    entries = calloc((size_t)(argc - 3), sizeof(*entries));
    if (entries == NULL) {
        fprintf(stderr, "mkfsimg: out of memory\n");
        return 1;
    }

    for (i = 3; i < argc; i++) {
        char *eq = strchr(argv[i], '=');
        FILE *fp;

        if (eq == NULL || eq == argv[i]) {
            fprintf(stderr, "mkfsimg: bad entry '%s', expected name=path\n", argv[i]);
            return 1;
        }
        if ((size_t)(eq - argv[i]) > TFS_NAME_MAX) {
            fprintf(stderr, "mkfsimg: name in '%s' exceeds %d bytes\n", argv[i], TFS_NAME_MAX);
            return 1;
        }

        *eq = '\0';
        entries[nentries].name = argv[i];
        entries[nentries].path = eq + 1;

        fp = fopen(entries[nentries].path, "rb");
        if (fp == NULL) {
            fprintf(stderr, "mkfsimg: cannot open '%s'\n", entries[nentries].path);
            return 1;
        }
        entries[nentries].size = file_size(fp);
        fclose(fp);
        if (entries[nentries].size < 0) {
            fprintf(stderr, "mkfsimg: cannot size '%s'\n", entries[nentries].path);
            return 1;
        }

        nentries++;
    }

    out = fopen(out_path, "wb");
    if (out == NULL) {
        fprintf(stderr, "mkfsimg: cannot create '%s'\n", out_path);
        return 1;
    }

    memcpy(hdr.th_magic, TFS_MAGIC, TFS_MAGIC_LEN);
    hdr.th_nfiles = (uint32_t)nentries;
    fwrite(&hdr, sizeof(hdr), 1, out);

    /* Every offset below, including the first, is aligned: the header
     * table's own size need not be a multiple of TFS_ALIGN, and treating
     * the first file as a special case that skips alignment would make
     * the writing loop below disagree with the offsets recorded here. */
    offset = (uint32_t)(sizeof(hdr) + (size_t)nentries * sizeof(struct tfs_dirent));
    offset = (offset + (TFS_ALIGN - 1)) & ~(uint32_t)(TFS_ALIGN - 1);
    for (i = 0; i < nentries; i++) {
        struct tfs_dirent de;

        memset(&de, 0, sizeof(de));
        strncpy(de.td_name, entries[i].name, TFS_NAME_MAX);
        de.td_offset = offset;
        de.td_size = (uint32_t)entries[i].size;
        fwrite(&de, sizeof(de), 1, out);

        entries[i].offset = offset;
        offset += (uint32_t)entries[i].size;
        offset = (offset + (TFS_ALIGN - 1)) & ~(uint32_t)(TFS_ALIGN - 1);
    }

    /* Pad up to the first entry's offset: it need not follow directly
     * behind the dirent table, per the alignment above. */
    while ((uint32_t)ftell(out) < entries[0].offset) {
        fputc(0, out);
    }

    for (i = 0; i < nentries; i++) {
        FILE *fp = fopen(entries[i].path, "rb");
        char buf[4096];
        size_t n;
        uint32_t next_offset;

        if (fp == NULL) {
            fprintf(stderr, "mkfsimg: cannot reopen '%s'\n", entries[i].path);
            return 1;
        }
        while ((n = fread(buf, 1, sizeof(buf), fp)) > 0) {
            fwrite(buf, 1, n, out);
        }
        fclose(fp);

        /* Pad up to the next entry's recorded offset, which is the same
         * alignment computation used above; recomputing it here from the
         * bytes just written, rather than reusing it, is what let the
         * two fall out of step in the first place when the file did not
         * start on an aligned boundary. */
        if (i + 1 < nentries) {
            next_offset = entries[i + 1].offset;
        } else {
            uint32_t end = entries[i].offset + (uint32_t)entries[i].size;
            next_offset = (end + (TFS_ALIGN - 1)) & ~(uint32_t)(TFS_ALIGN - 1);
        }
        while ((uint32_t)ftell(out) < next_offset) {
            fputc(0, out);
        }
    }

    fclose(out);
    free(entries);
    return 0;
}
