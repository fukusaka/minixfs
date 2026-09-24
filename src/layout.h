/*
 * SPDX-License-Identifier: BSD-2-Clause
 * Copyright (c) 2026 Shoichi Fukusaka
 *
 * layout.h - the on-disk layout of MINIX file systems, for the library.
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
#define SB12_STATE	18		/* 16, Linux: 1 if cleanly unmounted */
#define SB12_ZONES	20		/* 32, V2 only */

/* V3 super block. */
#define SB3_NINODES	0		/* 32 */
#define SB3_IMAP	6		/* 16 */
#define SB3_ZMAP	8		/* 16 */
#define SB3_FIRSTDATA	10		/* 16; 0 if it does not fit */
#define SB3_LOGZONE	12		/* 16 */
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

#endif /* LAYOUT_H */
