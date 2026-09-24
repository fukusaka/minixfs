/*
 * SPDX-License-Identifier: BSD-2-Clause
 * Copyright (c) 2026 Shoichi Fukusaka
 *
 * layout.h - the on-disk layout of MINIX file systems, and helpers that the
 * two halves of the library share.
 *
 * On-disk layout (block numbers in units of the block size):
 *
 *	block 0			boot block; the super block is at byte 1024
 *	block 2 ...		inode bit map (imap_blocks)
 *	...			zone bit map (zmap_blocks)
 *	...			inode table
 *	firstdatazone ...	data zones
 *
 * A zone is 2^log_zone_size blocks.  The versions differ as follows:
 *
 *			V1		V2		V3
 *	magic at	16		16		24
 *	block size	1024		1024		from the super block
 *	inode		32 bytes	64 bytes	64 bytes
 *	zone numbers	16-bit, 9	32-bit, 10	32-bit, 10
 *	dir entry	2 + 14 or 30	2 + 14 or 30	4 + 60
 *
 * The zone slots of an inode are 7 direct zones, then a single, a double
 * and (V2 and V3) a triple indirect zone.  MINIX itself never uses the
 * triple indirect zone, but Linux does.
 *
 * Everything is stored in the byte order of the machine that made the
 * file system: little-endian on the PC, big-endian on the 68000 (Atari
 * ST, Amiga, Macintosh).  The byte order is found from the magic number.
 *
 * The layouts follow fs/super.h, fs/inode.h and fs/type.h of MINIX 2.0.4
 * and minix/fs/mfs of MINIX 3.
 */

#ifndef LAYOUT_H
#define LAYOUT_H

#include <errno.h>
#include <unistd.h>

#include "mfs.h"

#define SUPER_OFFSET	1024		/* byte offset of the super block */
#define SUPER_SIZE	1024		/* bytes read for the super block */
#define START_BLOCK	2		/* first block of the inode map */
#define STATIC_BLOCK	1024		/* block size of V1 and V2 */
#define NR_DZONES	7		/* direct zones in any inode */
#define MAX_BLOCK	65536		/* largest V3 block size accepted */
#define MAX_LOG_ZONE	10		/* largest zone: 1024 blocks */

/* V1 and V2 super block: byte offsets and widths in bits. */
#define SB12_NINODES	0		/* 16 */
#define SB12_NZONES	2		/* 16, V1 only */
#define SB12_IMAP	4		/* 16 */
#define SB12_ZMAP	6		/* 16 */
#define SB12_FIRSTDATA	8		/* 16 */
#define SB12_LOGZONE	10		/* 16 */
#define SB12_MAXSIZE	12		/* 32 */
#define SB12_MAGIC	16		/* 16 */
#define SB12_STATE	18		/* 16, Linux: MFS_STATE_* */
#define SB12_ZONES	20		/* 32, V2 only */

/* V3 super block. */
#define SB3_NINODES	0		/* 32 */
#define SB3_IMAP	6		/* 16 */
#define SB3_ZMAP	8		/* 16 */
#define SB3_FIRSTDATA	10		/* 16; 0 if it does not fit */
#define SB3_LOGZONE	12		/* 16 */
#define SB3_FLAGS	14		/* 16, MINIX 3: MFS_FLAG_* */
#define SB3_MAXSIZE	16		/* 32 */
#define SB3_ZONES	20		/* 32 */
#define SB3_MAGIC	24		/* 16 */
#define SB3_BLOCKSIZE	28		/* 16 */

/* V1 inode, 32 bytes. */
#define I1_SIZE		32
#define I1_MODE		0		/* 16 */
#define I1_UID		2		/* 16 */
#define I1_FSIZE	4		/* 32 */
#define I1_MTIME	8		/* 32 */
#define I1_GID		12		/* 8 */
#define I1_NLINKS	13		/* 8 */
#define I1_ZONE		14		/* 9 x 16 */
#define I1_NZONES	9

/* V2 and V3 inode, 64 bytes. */
#define I2_SIZE		64
#define I2_MODE		0		/* 16 */
#define I2_NLINKS	2		/* 16 */
#define I2_UID		4		/* 16 */
#define I2_GID		6		/* 16 */
#define I2_FSIZE	8		/* 32 */
#define I2_ATIME	12		/* 32 */
#define I2_MTIME	16		/* 32 */
#define I2_CTIME	20		/* 32 */
#define I2_ZONE		24		/* 10 x 32 */
#define I2_NZONES	10

/* Store a number in the byte order of an image. */
static inline void
put16(enum mfs_order order, unsigned char *p, uint32_t v)
{
	if (order == MFS_BIG_ENDIAN) {
		p[0] = (unsigned char)(v >> 8);
		p[1] = (unsigned char)v;
	} else {
		p[0] = (unsigned char)v;
		p[1] = (unsigned char)(v >> 8);
	}
}

static inline void
put32(enum mfs_order order, unsigned char *p, uint32_t v)
{
	if (order == MFS_BIG_ENDIAN) {
		put16(order, p, v >> 16);
		put16(order, p + 2, v & 0xffff);
	} else {
		put16(order, p, v & 0xffff);
		put16(order, p + 2, v >> 16);
	}
}

/* Load a number stored in the byte order of an image. */
static inline uint32_t
load16(enum mfs_order order, const unsigned char *p)
{
	if (order == MFS_BIG_ENDIAN)
		return (uint32_t)p[0] << 8 | p[1];
	return (uint32_t)p[1] << 8 | p[0];
}

static inline uint32_t
load32(enum mfs_order order, const unsigned char *p)
{
	if (order == MFS_BIG_ENDIAN)
		return load16(order, p) << 16 | load16(order, p + 2);
	return load16(order, p + 2) << 16 | load16(order, p);
}

/* Write exactly len bytes at off. */
static inline int
write_at(int fd, const void *buf, size_t len, off_t off)
{
	const unsigned char *p;
	ssize_t n;

	p = buf;
	while (len > 0) {
		n = pwrite(fd, p, len, off);
		if (n == -1) {
			if (errno == EINTR)
				continue;
			return -errno;
		}
		p += n;
		len -= (size_t)n;
		off += n;
	}
	return 0;
}

#endif /* LAYOUT_H */
