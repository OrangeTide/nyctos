/*
 * SPDX-License-Identifier: 0BSD OR Apache-2.0
 *
 * PUBLIC DOMAIN (CC0-1.0)
 */

/*
 * Files, by name.
 *
 * A path is resolved against the namespace this process was given, per
 * section 20.3, and the walk goes to whatever handle it landed on. There
 * is no descriptor table and no ambient root: what comes back is a
 * handle, which is what authority looks like here, and a program holding
 * no directory can open nothing.
 *
 * The protocol underneath is section 25's and a program may speak it
 * directly, which three of them do. This is for the ones that only want
 * the bytes.
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
 * file: section 25.5 makes a short count a protocol outcome rather than
 * an error.
 */
sys_err_t file_read(sys_handle_t file, void *buf, uint32_t len,
  uint64_t off, uint32_t *got);

/* Tell the server, then drop the handle. Both, and in that order. */
sys_err_t file_close(sys_handle_t file);

#endif /* _FILE_H_ */
