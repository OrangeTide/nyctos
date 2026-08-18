/*
 * SPDX-License-Identifier: 0BSD OR Apache-2.0
 * Copyright (C) 2026 Jon Mayo <jon@rm-f.net>
 */

/*
 * The VFS layer proper. Today this is a thin forward to ramfs, the only
 * filesystem the kernel knows about; see the header comment in
 * sys/sys/vfs.h for what grows here later.
 */

#include <sys/vfs.h>

void
vfs_init(const void *initrd_start)
{
    ramfs_init(initrd_start);
}

int
vfs_lookup(const char *path, struct vnode *vp)
{
    if (*path == '/') {
        path++;
    }
    return ramfs_lookup(path, vp);
}
