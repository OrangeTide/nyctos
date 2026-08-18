/*
 * SPDX-License-Identifier: 0BSD OR Apache-2.0
 * Copyright (C) 2026 Jon Mayo <jon@rm-f.net>
 */

/*
 * The VFS layer. There is exactly one filesystem today, the ramdisk
 * driver in sys/kern/ramfs.c, so vfs_lookup() is a direct call into it
 * rather than a dispatch through a switch table. This is the seam where
 * a table of mounted filesystems goes if a second backing store (a real
 * block device, say) ever shows up; nothing above vfs_lookup() should
 * need to change when it does.
 *
 * A vnode is deliberately minimal: a read-only span of memory. There is
 * no MMU separating kernel and user address spaces yet (see
 * sys/arch/<arch>/machdep.S), so a vnode's data can be copied straight
 * into a user buffer with no copyin/copyout step.
 */

#ifndef _SYS_VFS_H_
#define _SYS_VFS_H_

typedef __SIZE_TYPE__ size_t;

struct vnode {
    const char *v_data;
    size_t      v_size;
};

/* sys/kern/ramfs.c */
void ramfs_init(const void *image);
int  ramfs_lookup(const char *path, struct vnode *vp);

/* sys/kern/vfs.c */
void vfs_init(const void *initrd_start);
int  vfs_lookup(const char *path, struct vnode *vp);

/* sys/kern/kern_file.c: the per-boot open file table. */
int  file_open(const char *path);
long file_read(int fd, void *buf, size_t nbytes);
int  file_close(int fd);

#endif /* _SYS_VFS_H_ */
