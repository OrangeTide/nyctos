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

/*
 * Sixteen blocks. The three files occupy the first seven; the rest are free,
 * where a write reaches a block no file holds, so the write tests and the
 * boot-to-boot marker touch the disk without disturbing a file.
 */
#define BLK_BLOCK_COUNT     16u

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

/*
 * A block for a file the filesystem server writes at run time rather than
 * reads from fixed bytes. It starts empty and takes whatever a write puts
 * in it, so it sits past the static files and past the blocks the store's
 * own write and persistence checks use (7 and 8).
 */
#define BLK_DATA_BLOCK      9u

/*
 * The on-disk directory of files made at run time, and the blocks their
 * content lives in. The directory is one block holding one entry, so a file
 * created in one boot is found in the next: the filesystem reads this block
 * at startup and writes it back when a file is made, grown, or truncated.
 * The content is a contiguous run, so a file's blocks are its start and the
 * blocks that follow, which keeps the on-disk record to a start and a length
 * and needs no list stored beside it.
 *
 * These step around the two blocks the store keeps for its own use: block 12
 * carries its snapshot-persistence demonstration and block 15 its in-boot
 * fork self-test, so the directory takes block 10 and the content run takes
 * 13 and 14.
 */
#define BLK_DIR_BLOCK       10u
#define BLK_CREATE_BASE     13u
#define BLK_CREATE_BLOCKS   2u      /* blocks 13..14 */

#endif /* _SYS_BLK_H_ */
