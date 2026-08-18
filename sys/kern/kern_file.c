/*
 * SPDX-License-Identifier: 0BSD OR Apache-2.0
 * Copyright (C) 2026 Jon Mayo <jon@rm-f.net>
 */

/*
 * The open file table. There is no process abstraction yet (see
 * kern/kern_exec.c), so this is a single global table rather than one per
 * process; it is the seam where a per-process fd table goes once there
 * is more than one process to own one. Descriptors 0-2 are reserved for
 * stdio, which sys_write() still handles directly, so allocation starts
 * at 3.
 */

#include <sys/vfs.h>

#define NFILE 8
#define FIRST_FD 3

struct file {
    int          f_inuse;
    struct vnode f_vnode;
    size_t       f_offset;
};

static struct file files[NFILE];

int
file_open(const char *path)
{
    struct vnode vn;
    int fd;

    if (vfs_lookup(path, &vn) != 0) {
        return -1;
    }

    for (fd = FIRST_FD; fd < NFILE; fd++) {
        if (!files[fd].f_inuse) {
            files[fd].f_inuse = 1;
            files[fd].f_vnode = vn;
            files[fd].f_offset = 0;
            return fd;
        }
    }
    return -1;
}

long
file_read(int fd, void *buf, size_t nbytes)
{
    struct file *f;
    char *dst = (char *)buf;
    const char *src;
    size_t remaining, n, i;

    if (fd < FIRST_FD || fd >= NFILE || !files[fd].f_inuse) {
        return -1;
    }
    f = &files[fd];

    remaining = f->f_vnode.v_size - f->f_offset;
    n = nbytes < remaining ? nbytes : remaining;

    src = f->f_vnode.v_data + f->f_offset;
    for (i = 0; i < n; i++) {
        dst[i] = src[i];
    }
    f->f_offset += n;

    return (long)n;
}

int
file_close(int fd)
{
    if (fd < FIRST_FD || fd >= NFILE || !files[fd].f_inuse) {
        return -1;
    }
    files[fd].f_inuse = 0;
    return 0;
}
