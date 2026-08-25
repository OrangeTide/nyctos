/*
 * SPDX-License-Identifier: 0BSD OR Apache-2.0
 *
 * PUBLIC DOMAIN (CC0-1.0)
 */

/*
 * What a block store is, for the layer above it.
 *
 * There is no protocol here, deliberately. A block read is a read: the
 * request and reply of section 25.2 already carry an offset, a length
 * and a buffer, and a block device is what you get when you drop the
 * name from that and require the offset and the length to be whole
 * blocks. Inventing a second message shape for the same operation would
 * have been a second thing to keep right for no gain.
 *
 * What is here is the block size, because it is the one number the two
 * sides have to agree on, and the shape of the disk, because the check
 * sequence puts the bytes there before either side runs.
 */

#ifndef _SYS_BLK_H_
#define _SYS_BLK_H_

#include <stdint.h>

#define BLK_BLOCK_BYTES     512u
#define BLK_BLOCK_COUNT     4u

/*
 * Where each of the filesystem's files starts. A file begins at a block
 * boundary because nothing here needs it not to, and a layout with no
 * reason behind it is a layout somebody will read meaning into.
 */
#define BLK_MOTD_BLOCK      0u
#define BLK_ALT_MOTD_BLOCK  1u
#define BLK_NOTES_BLOCK     2u

#endif /* _SYS_BLK_H_ */
