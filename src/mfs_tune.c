/*
 * SPDX-License-Identifier: BSD-2-Clause
 * Copyright (c) 2026 Shoichi Fukusaka
 *
 * mfs_tune.c - change MINIX file systems in place.
 *
 * Each change first reads everything it needs, with the file system as it
 * is, and only then writes, so that a failure to read leaves the file
 * system untouched.
 */

#include <errno.h>
#include <stdlib.h>
#include <string.h>

#include "layout.h"
#include "mfs.h"

/* A number in an on-disk structure: its byte offset and width. */
struct field {
	unsigned char	off;
	unsigned char	len;
};

static const struct field sb1_fields[] = {
	{ SB12_NINODES, 2 }, { SB12_NZONES, 2 }, { SB12_IMAP, 2 },
	{ SB12_ZMAP, 2 }, { SB12_FIRSTDATA, 2 }, { SB12_LOGZONE, 2 },
	{ SB12_MAXSIZE, 4 }, { SB12_MAGIC, 2 }, { SB12_STATE, 2 }
};

static const struct field sb2_fields[] = {
	{ SB12_NINODES, 2 }, { SB12_NZONES, 2 }, { SB12_IMAP, 2 },
	{ SB12_ZMAP, 2 }, { SB12_FIRSTDATA, 2 }, { SB12_LOGZONE, 2 },
	{ SB12_MAXSIZE, 4 }, { SB12_MAGIC, 2 }, { SB12_STATE, 2 },
	{ SB12_ZONES, 4 }
};

/* V3 also has a 16-bit zone count at 4 and padding at 26. */
static const struct field sb3_fields[] = {
	{ SB3_NINODES, 4 }, { 4, 2 }, { SB3_IMAP, 2 }, { SB3_ZMAP, 2 },
	{ SB3_FIRSTDATA, 2 }, { SB3_LOGZONE, 2 }, { SB3_FLAGS, 2 },
	{ SB3_MAXSIZE, 4 }, { SB3_ZONES, 4 }, { SB3_MAGIC, 2 }, { 26, 2 },
	{ SB3_BLOCKSIZE, 2 }
};

/* The zone slots follow these; gid and nlinks of V1 are single bytes. */
static const struct field i1_fields[] = {
	{ I1_MODE, 2 }, { I1_UID, 2 }, { I1_FSIZE, 4 }, { I1_MTIME, 4 }
};

static const struct field i2_fields[] = {
	{ I2_MODE, 2 }, { I2_NLINKS, 2 }, { I2_UID, 2 }, { I2_GID, 2 },
	{ I2_FSIZE, 4 }, { I2_ATIME, 4 }, { I2_MTIME, 4 }, { I2_CTIME, 4 }
};

#define NFIELDS(a)	(sizeof(a) / sizeof((a)[0]))

/* Reverse the n bytes at p. */
static void
swap(unsigned char *p, size_t n)
{
	unsigned char t;
	size_t i;

	for (i = 0; i < n / 2; i++) {
		t = p[i];
		p[i] = p[n - 1 - i];
		p[n - 1 - i] = t;
	}
}

static void
swap_fields(unsigned char *p, const struct field *f, size_t n)
{
	size_t i;

	for (i = 0; i < n; i++)
		swap(p + f[i].off, f[i].len);
}

/* Reverse each word of size bytes in n bytes at p. */
static void
swap_words(unsigned char *p, size_t n, size_t size)
{
	size_t i;

	for (i = 0; i + size <= n; i += size)
		swap(p + i, size);
}

static void
set_bit(unsigned char *bits, uint64_t n)
{
	bits[n / 8] |= (unsigned char)(1 << (n % 8));
}

static int
test_bit(const unsigned char *bits, uint64_t n)
{
	return (bits[n / 8] >> (n % 8)) & 1;
}

/* What mfs_convert_order() has to rewrite besides the fixed parts. */
struct convert {
	struct mfs	*fs;
	unsigned char	*indirect;	/* bit per zone: an indirect zone */
	unsigned char	*dirblock;	/* bit per block: directory data */
};

static int
note_indirect(uint32_t zone, int level, const struct mfs_zref *ref,
    void *arg)
{
	struct convert *cv;

	(void)ref;
	cv = arg;
	if (zone < cv->fs->firstdatazone || zone >= cv->fs->nzones)
		return MFS_WALK_SKIP;
	if (level > 0)
		set_bit(cv->indirect, zone);
	return 0;
}

/* Find the indirect zones of every file and the blocks of directories. */
static int
collect(struct convert *cv)
{
	struct mfs_inode ip;
	struct mfs *fs;
	uint32_t block, fblock, ino, n;
	int r;

	fs = cv->fs;
	for (ino = 1; ino <= fs->ninodes; ino++) {
		if ((r = mfs_get_inode(fs, ino, &ip)) < 0)
			return r;
		if (ip.mode == 0 || mfs_is_dev(&ip))
			continue;
		if ((r = mfs_walk_zones(fs, &ip, note_indirect, cv)) < 0)
			return r;
		if (!mfs_is_dir(&ip))
			continue;
		n = (ip.size + fs->block_size - 1) / fs->block_size;
		for (fblock = 0; fblock < n; fblock++) {
			if ((r = mfs_bmap(fs, &ip, fblock, &block)) < 0)
				return r;
			if (block != 0)
				set_bit(cv->dirblock, block);
		}
	}
	return 0;
}

/* Read a block, change it, and write it back. */
static int
rewrite(struct mfs *fs, uint32_t block, unsigned char *buf,
    void (*fn)(const struct mfs *, unsigned char *))
{
	int r;

	if ((r = mfs_read_block(fs, block, buf)) < 0)
		return r;
	fn(fs, buf);
	return mfs_write_block(fs, block, buf);
}

static void
swap_indirect(const struct mfs *fs, unsigned char *buf)
{
	swap_words(buf, fs->block_size, fs->zone_num_size);
}

static void
swap_dirblock(const struct mfs *fs, unsigned char *buf)
{
	uint32_t off;

	for (off = 0; off + fs->dirent_size <= fs->block_size;
	    off += fs->dirent_size)
		swap(buf + off, fs->dirent_ino);
}

static void
swap_map(const struct mfs *fs, unsigned char *buf)
{
	swap_words(buf, fs->block_size, fs->version == 3 ? 4 : 2);
}

/* Swap the inodes of an inode table block; first is its first inode. */
static void
swap_inodes(const struct mfs *fs, unsigned char *buf, uint32_t first)
{
	uint32_t i, per;
	unsigned char *p;

	per = fs->block_size / fs->inode_size;
	for (i = 0; i < per && first + i <= fs->ninodes; i++) {
		p = buf + i * fs->inode_size;
		if (fs->version == 1) {
			swap_fields(p, i1_fields, NFIELDS(i1_fields));
			swap_words(p + I1_ZONE, 2 * I1_NZONES, 2);
		} else {
			swap_fields(p, i2_fields, NFIELDS(i2_fields));
			swap_words(p + I2_ZONE, 4 * I2_NZONES, 4);
		}
	}
}

static int
swap_super(struct mfs *fs)
{
	unsigned char sb[SUPER_SIZE];
	int r;

	if ((r = mfs_read_device(fs, sb, sizeof(sb), SUPER_OFFSET)) < 0)
		return r;
	if (fs->version == 1)
		swap_fields(sb, sb1_fields, NFIELDS(sb1_fields));
	else if (fs->version == 2)
		swap_fields(sb, sb2_fields, NFIELDS(sb2_fields));
	else
		swap_fields(sb, sb3_fields, NFIELDS(sb3_fields));
	return mfs_write_device(fs, sb, sizeof(sb), SUPER_OFFSET);
}

int
mfs_convert_order(struct mfs *fs, enum mfs_order order)
{
	struct convert cv;
	unsigned char *buf;
	uint32_t b, block, first, itable, z;
	int r;

	if (!fs->writable)
		return -EROFS;
	if (fs->order == order)
		return 0;
	cv.fs = fs;
	cv.indirect = calloc((size_t)fs->nzones / 8 + 1, 1);
	cv.dirblock = calloc((size_t)fs->nblocks / 8 + 1, 1);
	buf = malloc(fs->block_size);
	if (cv.indirect == NULL || cv.dirblock == NULL || buf == NULL) {
		r = -ENOMEM;
		goto out;
	}
	if ((r = collect(&cv)) < 0)
		goto out;

	for (z = fs->firstdatazone; z < fs->nzones && r == 0; z++)
		if (test_bit(cv.indirect, z))
			r = rewrite(fs, z << fs->log_zone_size, buf,
			    swap_indirect);
	for (b = 0; b < fs->nblocks && r == 0; b++)
		if (test_bit(cv.dirblock, b))
			r = rewrite(fs, b, buf, swap_dirblock);
	for (b = 0; b < fs->imap_blocks + fs->zmap_blocks && r == 0; b++)
		r = rewrite(fs, START_BLOCK + b, buf, swap_map);
	itable = (uint32_t)(((uint64_t)fs->ninodes * fs->inode_size +
	    fs->block_size - 1) / fs->block_size);
	for (b = 0; b < itable && r == 0; b++) {
		block = fs->inode_start + b;
		first = b * (fs->block_size / fs->inode_size) + 1;
		if ((r = mfs_read_block(fs, block, buf)) < 0)
			break;
		swap_inodes(fs, buf, first);
		r = mfs_write_block(fs, block, buf);
	}
	if (r == 0 && (r = swap_super(fs)) == 0)
		fs->order = order;
out:
	free(cv.indirect);
	free(cv.dirblock);
	free(buf);
	return r;
}
