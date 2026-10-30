/* file.h : open, read, and close files by path.
 *
 * SPDX-License-Identifier: 0BSD OR Apache-2.0
 * Copyright (C) 2026 Jon Mayo <jon@rm-f.net>
 *
 * Declares the file calls of the C library: file_open and file_create to
 * reach a file by path, file_read and file_write to move its bytes,
 * file_truncate to set its length, file_stat to read its size and kind, and
 * file_close to release it. A path resolves through the process namespace.
 * A read or a write moves the bytes that fit in one message, which may be
 * fewer than asked for. Included by the file programs and the C library
 * file implementation.
 *
 */

#ifndef _FILE_H_
#define _FILE_H_

#include <os/sys.h>

#include <stddef.h>
#include <stdint.h>

/* Resolve a path, walk to what it names, and hand back the connection. */
sys_err_t file_open(const char *path, sys_handle_t *out);

/*
 * Resolve a path to the directory and leaf it names, make a file there, and
 * hand back a connection to it. The leaf is a single name, so a path with
 * nothing below its mount point is SYS_ERR_WRONG_TYPE the way file_open has
 * it, and a name a create finds already taken is SYS_ERR_INVALID_ARG.
 */
sys_err_t file_create(const char *path, sys_handle_t *out);

/*
 * Write bytes at an offset, growing the file where the write reaches past
 * its end. What the call carries is bounded by the inline payload, so a
 * caller with more to write makes more calls; put reports what this one
 * took. A short count is not an error.
 */
sys_err_t file_write(sys_handle_t file, const void *buf, uint32_t len,
  uint64_t off, uint32_t *put);

/* Set the file's length, dropping any bytes above the new one. */
sys_err_t file_truncate(sys_handle_t file, uint64_t size);

/*
 * The size and kind of the node the handle names, either output optional.
 * The kind is FS_KIND_FILE or FS_KIND_DIR from sys/fs.h.
 */
sys_err_t file_stat(sys_handle_t file, uint64_t *size, uint32_t *kind);

/*
 * Bytes at an offset. What comes back is what fitted in one message,
 * which may be less than was asked for and is zero at the end of the
 * file: a short count is a protocol outcome rather than
 * an error.
 */
sys_err_t file_read(sys_handle_t file, void *buf, uint32_t len,
  uint64_t off, uint32_t *got);

/* Tell the server, then drop the handle. Both, and in that order. */
sys_err_t file_close(sys_handle_t file);

#endif /* _FILE_H_ */
