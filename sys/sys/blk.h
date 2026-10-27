/* blk.h : block size and disk layout constants.
 *
 * SPDX-License-Identifier: 0BSD OR Apache-2.0
 * Copyright (C) 2026 Jon Mayo <jon@rm-f.net>
 *
 * Defines the block size (BLK_BLOCK_BYTES), the block count
 * (BLK_BLOCK_COUNT), and the starting block and byte length of each file
 * on the disk. The header defines no protocol: block reads use the file
 * read request with block-aligned offsets. Included by the block server,
 * the filesystem server, and the kernel init code.
 *
 */

#ifndef _SYS_BLK_H_
#define _SYS_BLK_H_

#include <stdint.h>

/*
 * Sixteen bytes, which is smaller than every file on the disk and is
 * chosen to be.
 *
 * A block size larger than the data is a block store that never asks
 * the layer above it to do anything: one read, one block, no
 * arithmetic, and a loop that runs once whatever it is given. What
 * makes two layers two is that the upper one has to turn a byte range
 * into a set of blocks, so the block has to be small enough that it
 * usually does. Every file here spans two or three, and a read of one
 * fetches each in turn and copies part of the last.
 *
 * It is not a plausible size for a disk. It is the size that makes the
 * layering real in a system whose entire contents are three short
 * strings, and the alternative was a loop nothing ever entered twice.
 */
#define BLK_BLOCK_BYTES     16u
#define BLK_BLOCK_COUNT     8u

/*
 * Where each file starts and how long it is. A file begins at a block
 * boundary because nothing here needs it not to, and a layout with no
 * reason behind it is a layout somebody will read meaning into.
 */
#define BLK_MOTD_BLOCK      0u
#define BLK_MOTD_BYTES      31u

#define BLK_ALT_MOTD_BLOCK  2u
#define BLK_ALT_MOTD_BYTES  32u

#define BLK_NOTES_BLOCK     4u
#define BLK_NOTES_BYTES     35u

#endif /* _SYS_BLK_H_ */
