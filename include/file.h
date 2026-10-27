/* file.h : open, read, and close files by path.
 *
 * SPDX-License-Identifier: 0BSD OR Apache-2.0
 * Copyright (C) 2026 Jon Mayo <jon@rm-f.net>
 *
 * Declares file_open(), file_read(), and file_close() for the C library.
 * file_open() resolves a path through the process namespace and returns
 * a handle. file_read() returns the bytes that fit in one message, which
 * may be fewer than requested, and zero at end of file. Included by the
 * cat program and the C library file implementation.
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
