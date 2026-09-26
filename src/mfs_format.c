/*
 * SPDX-License-Identifier: BSD-2-Clause
 * Copyright (c) 2026 Shoichi Fukusaka
 *
 * mfs_format.c - make new, empty MINIX file systems.
 *
 * mfs_plan() works out the layout: the inode and zone maps are just large
 * enough for a bit per inode and per zone (bit 0 of each is never used),
 * the inode table follows them, and the data zones start at the first
 * zone after the inode table.  mfs_format() writes the super block, the
 * maps, the inode table and a root directory holding "." and "..".
 *
 * A file system of Minix-vmd with flex directories is laid out as that
 * of MINIX of its version, with the magic number of names of 14, and
 * differs as the mkfs of Minix-vmd makes it: the super block has the
 * zone size in one byte, the flags in the next and 0x7f, 0x13 where
 * Linux keeps its state, and "." and ".." take a slot each.
 *
 * The first 1024 bytes of the device, where a boot block may live, are
 * left alone.
 */

#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "layout.h"
#include "mfs.h"

#define V3_BLOCK	4096		/* default V3 block size */
#define MAX_V3_BLOCK	32768		/* largest V3 block size made */
#define V2_MAX_SIZE	0x7fffffff	/* s_max_size of V2 and V3 */
#define ROOT_MODE	0040755
#define MAX_16		0xffff

/* Block size and name length, from the parameters and the version. */
static int
plan_sizes(const struct mfs_params *p, struct mfs_layout *l)
{
	if (p->version < 1 || p->version > 3 || (p->flex && p->version == 3))
		return -EINVAL;
	if (p->version == 3) {
		l->block_size = p->block_size != 0 ? p->block_size : V3_BLOCK;
		if (l->block_size < STATIC_BLOCK ||
		    l->block_size > MAX_V3_BLOCK ||
		    l->block_size % STATIC_BLOCK != 0)
			return -EINVAL;
		if (p->namelen != 0 && p->namelen != 60)
			return -EINVAL;
		l->namelen = 60;
	} else {
		if (p->block_size != 0 && p->block_size != STATIC_BLOCK)
			return -EINVAL;
		l->block_size = STATIC_BLOCK;
		l->namelen = p->namelen != 0 ? p->namelen : p->flex ? 60 : 14;
		if (p->flex ? l->namelen != 60 :
		    l->namelen != 14 && l->namelen != 30)
			return -EINVAL;
	}
	if (p->log_zone_size > MAX_LOG_ZONE)
		return -EINVAL;
	return 0;
}

/*
 * The number of inodes: as asked, or about one for every three blocks,
 * rounded up to fill the blocks of the inode table.
 */
static int
plan_inodes(const struct mfs_params *p, struct mfs_layout *l)
{
	uint64_t max, n, per;

	max = p->version == 3 ? UINT32_MAX - 1 : MAX_16;
	if (p->ninodes != 0) {
		if (p->ninodes > max)
			return -EFBIG;
		l->ninodes = p->ninodes;
		return 0;
	}
	per = l->block_size / (p->version == 1 ? I1_SIZE : I2_SIZE);
	n = (l->nblocks / 3 + per - 1) / per * per;
	if (n == 0)
		n = per;
	l->ninodes = (uint32_t)(n < max ? n : max);
	return 0;
}

int
mfs_plan(const struct mfs_params *p, struct mfs_layout *l)
{
	uint64_t bits, first, itable, maps;
	uint32_t isize;
	int r;

	(void)memset(l, 0, sizeof(*l));
	if ((r = plan_sizes(p, l)) < 0)
		return r;
	l->nzones = p->nblocks >> p->log_zone_size;
	if (l->nzones == 0)
		return -ENOSPC;
	if (p->version == 1 && l->nzones > MAX_16)
		return -EFBIG;
	l->nblocks = l->nzones << p->log_zone_size;
	if ((r = plan_inodes(p, l)) < 0)
		return r;

	bits = (uint64_t)l->block_size * 8;
	maps = ((uint64_t)l->ninodes + bits) / bits;
	if (maps > MAX_16)
		return -EFBIG;
	l->imap_blocks = (uint32_t)maps;
	maps = ((uint64_t)l->nzones + bits) / bits;
	if (maps > MAX_16)
		return -EFBIG;
	l->zmap_blocks = (uint32_t)maps;
	l->inode_start = START_BLOCK + l->imap_blocks + l->zmap_blocks;

	isize = p->version == 1 ? I1_SIZE : I2_SIZE;
	itable = ((uint64_t)l->ninodes * isize + l->block_size - 1) /
	    l->block_size;
	l->itable_blocks = (uint32_t)itable;
	first = (l->inode_start + itable + ((uint64_t)1 << p->log_zone_size) -
	    1) >> p->log_zone_size;
	/* One zone must be left for the root directory. */
	if (first + 1 > l->nzones)
		return -ENOSPC;
	if (p->version != 3 && first > MAX_16)
		return -EFBIG;
	l->firstdatazone = (uint32_t)first;

	l->max_size = p->max_size != 0 ? p->max_size :
	    mfs_minix_size(p->version, l->block_size, p->log_zone_size);
	if (p->version == 1) {
		l->magic = l->namelen == 30 ? MFS_MAGIC_V1L : MFS_MAGIC_V1;
	} else {
		if (p->version == 3)
			l->magic = MFS_MAGIC_V3;
		else
			l->magic = l->namelen == 30 ? MFS_MAGIC_V2L :
			    MFS_MAGIC_V2;
	}
	return 0;
}

int
mfs_linux_only(int version, uint32_t namelen)
{
	return version != 3 && namelen == 30;
}

int
mfs_default_map_end(int version, uint32_t namelen)
{
	return mfs_linux_only(version, namelen);
}

uint32_t
mfs_linux_max_size(int version, uint32_t log_zone_size)
{
	uint64_t bytes;

	if (version != 1)
		return V2_MAX_SIZE;
	/* What the zone slots reach: 16-bit zone numbers. */
	bytes = (NR_DZONES + 512 + 512 * 512) *
	    ((uint64_t)STATIC_BLOCK << log_zone_size);
	return bytes < V2_MAX_SIZE ? (uint32_t)bytes : V2_MAX_SIZE;
}

static void
fill_super(const struct mfs_params *p, const struct mfs_layout *l,
    unsigned char *sb)
{
	enum mfs_order o;

	o = p->order;
	if (p->version == 3) {
		put32(o, sb + SB3_NINODES, l->ninodes);
		put16(o, sb + SB3_IMAP, l->imap_blocks);
		put16(o, sb + SB3_ZMAP, l->zmap_blocks);
		/* MINIX 3 writes 0 when the number does not fit. */
		put16(o, sb + SB3_FIRSTDATA, l->firstdatazone <= MAX_16 ?
		    l->firstdatazone : 0);
		put16(o, sb + SB3_LOGZONE, p->log_zone_size);
		put16(o, sb + SB3_FLAGS, MFS_FLAG_CLEAN);
		put32(o, sb + SB3_MAXSIZE, l->max_size);
		put32(o, sb + SB3_ZONES, l->nzones);
		put16(o, sb + SB3_MAGIC, l->magic);
		put16(o, sb + SB3_BLOCKSIZE, l->block_size);
		return;
	}
	put16(o, sb + SB12_NINODES, l->ninodes);
	put16(o, sb + SB12_NZONES, p->version == 1 ? l->nzones : 0);
	put16(o, sb + SB12_IMAP, l->imap_blocks);
	put16(o, sb + SB12_ZMAP, l->zmap_blocks);
	put16(o, sb + SB12_FIRSTDATA, l->firstdatazone);
	put16(o, sb + SB12_LOGZONE, p->log_zone_size);
	put32(o, sb + SB12_MAXSIZE, l->max_size);
	put16(o, sb + SB12_MAGIC, l->magic);
	if (p->flex) {
		sb[SBVMD_LOGZONE] = (unsigned char)p->log_zone_size;
		sb[SBVMD_FLAGS] = MFS_VMD_FLEX | MFS_VMD_CLEAN;
		sb[SBVMD_MAGIC] = SBVMD_MAGIC0;
		sb[SBVMD_MAGIC + 1] = SBVMD_MAGIC1;
	} else
		put16(o, sb + SB12_STATE, MFS_STATE_VALID);
	if (p->version == 2)
		put32(o, sb + SB12_ZONES, l->nzones);
}

/*
 * Write a bit map of nblocks blocks at block start in which bits 0 to
 * used are set, bits up to last are clear, and the bits after last are
 * as the parameters ask.  The maps are arrays of words, 16 bits wide in
 * V1 and V2 and 32 bits in V3, in the byte order of the image.
 */
static int
write_map(int fd, const struct mfs_params *p, const struct mfs_layout *l,
    unsigned char *buf, uint32_t start, uint32_t nblocks, uint32_t used,
    uint32_t last)
{
	uint64_t bit;
	uint32_t b, byte, i, per;
	int r;

	per = l->block_size * 8;
	for (b = 0; b < nblocks; b++) {
		(void)memset(buf, 0, l->block_size);
		for (i = 0; i < per; i++) {
			bit = (uint64_t)b * per + i;
			if (bit <= used || (bit > last && p->map_end)) {
				byte = i / 8;
				if (p->order == MFS_BIG_ENDIAN)
					byte ^= p->version == 3 ? 3 : 1;
				buf[byte] |= (unsigned char)(1 << (i % 8));
			}
		}
		r = write_at(fd, buf, l->block_size,
		    (off_t)(start + b) * l->block_size);
		if (r < 0)
			return r;
	}
	return 0;
}

/* The inode of the root directory, at p. */
static void
fill_root(const struct mfs_params *p, const struct mfs_layout *l,
    unsigned char *ip)
{
	enum mfs_order o;
	uint32_t size;

	o = p->order;
	size = p->flex ? 2 * FLEX_SLOT :
	    2 * ((p->version == 3 ? 4 : 2) + l->namelen);
	if (p->version == 1) {
		put16(o, ip + I1_MODE, ROOT_MODE);
		put32(o, ip + I1_FSIZE, size);
		put32(o, ip + I1_MTIME, p->time);
		ip[I1_NLINKS] = 2;
		put16(o, ip + I1_ZONE, l->firstdatazone);
		return;
	}
	put16(o, ip + I2_MODE, ROOT_MODE);
	put16(o, ip + I2_NLINKS, 2);
	put32(o, ip + I2_FSIZE, size);
	put32(o, ip + I2_ATIME, p->time);
	put32(o, ip + I2_MTIME, p->time);
	put32(o, ip + I2_CTIME, p->time);
	put32(o, ip + I2_ZONE, l->firstdatazone);
}

/* The root directory: "." and "..", both inode 1. */
static void
fill_root_dir(const struct mfs_params *p, const struct mfs_layout *l,
    unsigned char *d)
{
	uint32_t ino, size;

	if (p->flex) {
		put16(p->order, d, MFS_ROOT_INO);
		d[FLEX_NAME] = '.';
		put16(p->order, d + FLEX_SLOT, MFS_ROOT_INO);
		d[FLEX_SLOT + FLEX_NAME] = '.';
		d[FLEX_SLOT + FLEX_NAME + 1] = '.';
		return;
	}
	ino = p->version == 3 ? 4 : 2;
	size = ino + l->namelen;
	if (ino == 4) {
		put32(p->order, d, MFS_ROOT_INO);
		put32(p->order, d + size, MFS_ROOT_INO);
	} else {
		put16(p->order, d, MFS_ROOT_INO);
		put16(p->order, d + size, MFS_ROOT_INO);
	}
	d[ino] = '.';
	d[size + ino] = '.';
	d[size + ino + 1] = '.';
}

/* Zero everything from byte SUPER_OFFSET to the end of the root zone. */
static int
clear_metadata(int fd, const struct mfs_params *p,
    const struct mfs_layout *l, unsigned char *buf)
{
	uint64_t end, off;
	size_t n;
	int r;

	(void)memset(buf, 0, l->block_size);
	end = ((uint64_t)l->firstdatazone + 1) << p->log_zone_size;
	end *= l->block_size;
	for (off = SUPER_OFFSET; off < end; off += n) {
		n = l->block_size - off % l->block_size;
		if ((r = write_at(fd, buf, n, (off_t)off)) < 0)
			return r;
	}
	return 0;
}

int
mfs_format(int fd, const struct mfs_params *p, const struct mfs_layout *l)
{
	unsigned char *buf;
	off_t off;
	int r;

	if ((buf = malloc(l->block_size)) == NULL)
		return -ENOMEM;
	if ((r = clear_metadata(fd, p, l, buf)) < 0)
		goto out;

	(void)memset(buf, 0, l->block_size);
	fill_super(p, l, buf);
	if ((r = write_at(fd, buf, SUPER_SIZE, SUPER_OFFSET)) < 0)
		goto out;

	r = write_map(fd, p, l, buf, START_BLOCK, l->imap_blocks, 1,
	    l->ninodes);
	if (r < 0)
		goto out;
	r = write_map(fd, p, l, buf, START_BLOCK + l->imap_blocks,
	    l->zmap_blocks, 1, l->nzones - l->firstdatazone);
	if (r < 0)
		goto out;

	(void)memset(buf, 0, l->block_size);
	fill_root(p, l, buf);
	off = (off_t)l->inode_start * l->block_size;
	if ((r = write_at(fd, buf, p->version == 1 ? I1_SIZE : I2_SIZE,
	    off)) < 0)
		goto out;

	(void)memset(buf, 0, l->block_size);
	fill_root_dir(p, l, buf);
	off = ((off_t)l->firstdatazone << p->log_zone_size) * l->block_size;
	r = write_at(fd, buf, l->block_size, off);
out:
	free(buf);
	return r;
}
