/*
 * SPDX-License-Identifier: BSD-2-Clause
 * Copyright (c) 2026 Shoichi Fukusaka
 *
 * mfs_write.c - write files into a MINIX file system: take inodes and
 * zones, write file data through direct and indirect zones, and add
 * directory entries.
 *
 * The bit maps are read on the first allocation and kept in memory until
 * mfs_sync().  A search for a free inode or zone starts where the last
 * one ended, since a file system being filled takes them in order.
 */

#include <errno.h>
#include <stdlib.h>
#include <string.h>

#include "layout.h"
#include "mfs.h"

/* Read both bit maps, the first time they are needed. */
static int
load_maps(struct mfs *fs)
{
	int r;

	if (!fs->writable)
		return -EROFS;
	if (fs->imap != NULL)
		return 0;
	if ((r = mfs_load_map(fs, MFS_IMAP, &fs->imap)) < 0)
		return r;
	if ((r = mfs_load_map(fs, MFS_ZMAP, &fs->zmap)) < 0) {
		free(fs->imap);
		fs->imap = NULL;
		return r;
	}
	fs->inext = 1;
	fs->znext = 1;
	return 0;
}

/*
 * Find a clear bit among bits 1 to n of map, from *next on and then from
 * the start; set it and return it, or 0 if there is none.
 */
static uint32_t
take_bit(const struct mfs *fs, unsigned char *map, uint32_t n,
    uint32_t *next)
{
	uint32_t bit, i;

	for (i = 0; i < n; i++) {
		bit = (*next - 1 + i) % n + 1;
		if (!mfs_map_bit(fs, map, bit)) {
			mfs_set_map_bit(fs, map, bit, 1);
			*next = bit % n + 1;
			return bit;
		}
	}
	return 0;
}

int
mfs_alloc_inode(struct mfs *fs, uint32_t *ino)
{
	uint32_t bit;
	int r;

	if ((r = load_maps(fs)) < 0)
		return r;
	if ((bit = take_bit(fs, fs->imap, fs->ninodes, &fs->inext)) == 0)
		return -ENOSPC;
	*ino = bit;
	return 0;
}

int
mfs_alloc_zone(struct mfs *fs, uint32_t *zone)
{
	uint32_t bit, i, z;
	int r;

	if ((r = load_maps(fs)) < 0)
		return r;
	bit = take_bit(fs, fs->zmap, fs->nzones - fs->firstdatazone,
	    &fs->znext);
	if (bit == 0)
		return -ENOSPC;
	z = fs->firstdatazone + bit - 1;
	(void)memset(fs->dbuf, 0, fs->block_size);
	for (i = 0; i < 1U << fs->log_zone_size; i++) {
		r = mfs_write_block(fs, (z << fs->log_zone_size) + i,
		    fs->dbuf);
		if (r < 0)
			return r;
	}
	*zone = z;
	return 0;
}

int
mfs_sync(struct mfs *fs)
{
	int r;

	if (fs->imap == NULL)
		return 0;
	if ((r = mfs_store_map(fs, MFS_IMAP, fs->imap)) < 0)
		return r;
	return mfs_store_map(fs, MFS_ZMAP, fs->zmap);
}

/*
 * The zone number at entry index of indirect zone ind; with a zone to
 * put there, take a new zone and store it if there is none yet.
 */
static int
indirect_entry(struct mfs *fs, uint32_t ind, uint32_t index, int take,
    uint32_t *zone)
{
	unsigned char *p;
	uint32_t block;
	int r;

	block = ind << fs->log_zone_size;
	if ((r = mfs_read_block(fs, block, fs->ibuf)) < 0)
		return r;
	p = fs->ibuf + index * fs->zone_num_size;
	*zone = fs->zone_num_size == 2 ? load16(fs->order, p) :
	    load32(fs->order, p);
	if (*zone != 0 || !take)
		return 0;
	/* mfs_alloc_zone() uses the other buffer. */
	if ((r = mfs_alloc_zone(fs, zone)) < 0)
		return r;
	if ((r = mfs_read_block(fs, block, fs->ibuf)) < 0)
		return r;
	if (fs->zone_num_size == 2)
		put16(fs->order, p, *zone);
	else
		put32(fs->order, p, *zone);
	return mfs_write_block(fs, block, fs->ibuf);
}

/*
 * The zone that holds zone number n of the file *ip, taken if there is
 * none and take is set; 0 in *zone for a hole.
 */
static int
file_zone(struct mfs *fs, struct mfs_inode *ip, uint64_t n, int take,
    uint32_t *zone)
{
	uint64_t per;
	uint32_t level, slot;
	int r;

	if (n < fs->ndzones) {
		if (ip->zone[n] == 0 && take &&
		    (r = mfs_alloc_zone(fs, &ip->zone[n])) < 0)
			return r;
		*zone = ip->zone[n];
		return 0;
	}
	n -= fs->ndzones;
	per = 1;
	for (level = 1; level <= fs->nlevels; level++) {
		per *= fs->nindirs;
		if (n < per)
			break;
		n -= per;
	}
	if (level > fs->nlevels)
		return -EFBIG;
	slot = fs->ndzones + level - 1;
	if (ip->zone[slot] == 0 && take &&
	    (r = mfs_alloc_zone(fs, &ip->zone[slot])) < 0)
		return r;
	*zone = ip->zone[slot];
	while (level-- > 0 && *zone != 0) {
		per /= fs->nindirs;
		r = indirect_entry(fs, *zone, (uint32_t)(n / per), take,
		    zone);
		if (r < 0)
			return r;
		n %= per;
	}
	return 0;
}

static int
all_zero(const unsigned char *p, size_t n)
{
	size_t i;

	for (i = 0; i < n; i++)
		if (p[i] != 0)
			return 0;
	return 1;
}

int
mfs_pwrite(struct mfs *fs, struct mfs_inode *ip, const void *buf,
    size_t len, uint32_t off)
{
	const unsigned char *p;
	uint64_t end, pos;
	uint32_t block, in, n, zone;
	int r;

	if ((r = load_maps(fs)) < 0)
		return r;
	end = (uint64_t)off + len;
	if (end > fs->max_file || end > UINT32_MAX)
		return -EFBIG;
	p = buf;
	for (pos = off; pos < end; pos += n, p += n) {
		in = (uint32_t)(pos % fs->block_size);
		n = fs->block_size - in;
		if (n > end - pos)
			n = (uint32_t)(end - pos);
		block = (uint32_t)(pos / fs->block_size);
		/* Zeros into a hole leave the hole. */
		r = file_zone(fs, ip, block >> fs->log_zone_size,
		    !all_zero(p, n), &zone);
		if (r < 0)
			return r;
		if (zone == 0)
			continue;
		block = (zone << fs->log_zone_size) +
		    (block & ((1U << fs->log_zone_size) - 1));
		if (n < fs->block_size &&
		    (r = mfs_read_block(fs, block, fs->dbuf)) < 0)
			return r;
		(void)memcpy(fs->dbuf + in, p, n);
		if ((r = mfs_write_block(fs, block, fs->dbuf)) < 0)
			return r;
	}
	if (end > ip->size)
		ip->size = (uint32_t)end;
	return 0;
}

/* The offset of the first entry not in use, or of the end. */
struct free_slot {
	uint32_t	off;
	uint32_t	size;		/* of an entry */
};

static int
slot_fn(const struct mfs_dirent *de, void *arg)
{
	struct free_slot *s;

	s = arg;
	/* Entries in use come in order: the first gap is free. */
	if (de->off != s->off)
		return 1;
	s->off += s->size;
	return 0;
}

int
mfs_add_entry(struct mfs *fs, struct mfs_inode *dp, const char *name,
    uint32_t ino)
{
	unsigned char entry[4 + MFS_MAX_NAME];
	struct free_slot s;
	size_t len;
	int r;

	/* Entries of flex directories are not all the same size. */
	if (fs->flex)
		return -ENOTSUP;
	if ((len = strlen(name)) > fs->namelen)
		return -ENAMETOOLONG;
	(void)memset(entry, 0, sizeof(entry));
	if (fs->dirent_ino == 2)
		put16(fs->order, entry, ino);
	else
		put32(fs->order, entry, ino);
	(void)memcpy(entry + fs->dirent_ino, name, len);
	s.off = 0;
	s.size = fs->dirent_size;
	if ((r = mfs_readdir(fs, dp, slot_fn, &s)) < 0)
		return r;
	return mfs_pwrite(fs, dp, entry, fs->dirent_size, s.off);
}
