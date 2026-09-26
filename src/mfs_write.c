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
 * the start; set it and return it, or 0 if there is none.  The sum is
 * taken in 64 bits, as *next - 1 + i passes 2^32 where n is above 2^31.
 */
static uint32_t
take_bit(const struct mfs *fs, unsigned char *map, uint32_t n,
    uint32_t *next)
{
	uint64_t i;
	uint32_t bit;

	for (i = 0; i < n; i++) {
		bit = (uint32_t)(((uint64_t)*next - 1 + i) % n + 1);
		if (!mfs_map_bit(fs, map, bit)) {
			mfs_set_map_bit(fs, map, bit, 1);
			*next = (uint32_t)((uint64_t)bit % n + 1);
			return bit;
		}
	}
	return 0;
}

/*
 * With maps_through, write the block of a map that holds bit at once,
 * rather than at mfs_sync().
 */
static int
map_changed(struct mfs *fs, enum mfs_map which, uint32_t bit)
{
	const unsigned char *map;
	uint32_t block, start;

	if (!fs->maps_through)
		return 0;
	block = bit / (fs->block_size * 8);
	if (which == MFS_IMAP) {
		map = fs->imap;
		start = START_BLOCK;
	} else {
		map = fs->zmap;
		start = START_BLOCK + fs->imap_blocks;
	}
	return mfs_write_block(fs, start + block,
	    map + (size_t)block * fs->block_size);
}

void
mfs_maps_through(struct mfs *fs, int on)
{
	fs->maps_through = on;
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
	return map_changed(fs, MFS_IMAP, bit);
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
	if ((r = map_changed(fs, MFS_ZMAP, bit)) < 0)
		return r;
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
mfs_free_inode(struct mfs *fs, uint32_t ino)
{
	int r;

	if ((r = load_maps(fs)) < 0)
		return r;
	if (ino == 0 || ino > fs->ninodes)
		return -EIO;
	mfs_set_map_bit(fs, fs->imap, ino, 0);
	return map_changed(fs, MFS_IMAP, ino);
}

int
mfs_free_zone(struct mfs *fs, uint32_t zone)
{
	int r;

	if ((r = load_maps(fs)) < 0)
		return r;
	if (zone < fs->firstdatazone || zone >= fs->nzones)
		return -EIO;
	mfs_set_map_bit(fs, fs->zmap, zone - fs->firstdatazone + 1, 0);
	return map_changed(fs, MFS_ZMAP, zone - fs->firstdatazone + 1);
}

static int
free_fn(uint32_t zone, int level, const struct mfs_zref *ref, void *arg)
{
	struct mfs *fs;

	(void)level;
	(void)ref;
	fs = arg;
	/* A number outside the data area names no zone of this file. */
	if (zone < fs->firstdatazone || zone >= fs->nzones)
		return 0;
	return mfs_free_zone(fs, zone);
}

int
mfs_truncate(struct mfs *fs, struct mfs_inode *ip)
{
	uint32_t i;
	int r;

	if ((r = load_maps(fs)) < 0)
		return r;
	/* The zone of a device holds its number, not data. */
	if (!mfs_is_dev(ip) && (r = mfs_walk_zones(fs, ip, free_fn, fs)) < 0)
		return r;
	for (i = 0; i < MFS_NR_ZONES; i++)
		ip->zone[i] = 0;
	ip->size = 0;
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
 * The zone number at entry index of indirect zone ind, which is in the
 * data area; with a zone to put there, take a new zone and store it if
 * there is none yet.  A number outside the data area is -EIO, as when
 * reading, and nothing is written through it.
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
	if ((r = check_zone(fs, *zone)) < 0)
		return r;
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
 * none and take is set; 0 in *zone for a hole.  A zone number on the way
 * that lies outside the data area is -EIO: a damaged file is left to
 * fsck_minixfs, and neither written through nor given a new zone.
 */
static int
file_zone(struct mfs *fs, struct mfs_inode *ip, uint64_t n, int take,
    uint32_t *zone)
{
	uint64_t per;
	uint32_t level, slot;
	int r;

	if (n < fs->ndzones) {
		if ((r = check_zone(fs, ip->zone[n])) < 0)
			return r;
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
	if ((r = check_zone(fs, ip->zone[slot])) < 0)
		return r;
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

static uint32_t
get_ref(const struct mfs *fs, const unsigned char *p)
{
	return fs->zone_num_size == 2 ? load16(fs->order, p) :
	    load32(fs->order, p);
}

static void
put_ref(const struct mfs *fs, unsigned char *p, uint32_t zone)
{
	if (fs->zone_num_size == 2)
		put16(fs->order, p, zone);
	else
		put32(fs->order, p, zone);
}

/* Free a zone of a file; a number outside the data area is dropped. */
static int
drop_zone(struct mfs *fs, uint32_t zone)
{
	if (zone < fs->firstdatazone || zone >= fs->nzones)
		return 0;
	return mfs_free_zone(fs, zone);
}

/*
 * Free what the indirect zone *zp of the given level lists from file zone
 * keep on, counted from the first zone it covers, and the indirect zone
 * itself if nothing is left in it.  Each level reads into a buffer of
 * its own, since the levels below use the one of the file system.
 */
static int
trunc_indirect(struct mfs *fs, uint32_t *zp, uint32_t level, uint64_t keep)
{
	unsigned char *buf, *p;
	uint64_t per, start;
	uint32_t i, sub, z;
	int changed, r, used;

	if (*zp < fs->firstdatazone || *zp >= fs->nzones) {
		*zp = 0;
		return 0;
	}
	for (per = 1, i = 1; i < level; i++)
		per *= fs->nindirs;
	if ((buf = malloc(fs->block_size)) == NULL)
		return -ENOMEM;
	if ((r = mfs_read_block(fs, *zp << fs->log_zone_size, buf)) < 0)
		goto out;
	changed = used = 0;
	for (i = 0; i < fs->nindirs && r == 0; i++) {
		p = buf + (size_t)i * fs->zone_num_size;
		if ((z = get_ref(fs, p)) == 0)
			continue;
		start = (uint64_t)i * per;
		if (start + per <= keep) {
			used = 1;
			continue;
		}
		sub = z;
		if (level == 1) {
			r = drop_zone(fs, z);
			sub = 0;
		} else {
			r = trunc_indirect(fs, &sub, level - 1,
			    keep > start ? keep - start : 0);
		}
		if (sub != z) {
			put_ref(fs, p, sub);
			changed = 1;
		}
		used |= sub != 0;
	}
	if (r == 0 && !used) {
		r = drop_zone(fs, *zp);
		*zp = 0;
	} else if (r == 0 && changed) {
		r = mfs_write_block(fs, *zp << fs->log_zone_size, buf);
	}
out:
	free(buf);
	return r;
}

/* Free the zones of the file *ip from file zone keep on. */
static int
free_from(struct mfs *fs, struct mfs_inode *ip, uint64_t keep)
{
	uint64_t base, per;
	uint32_t i, level, slot;
	int r;

	for (i = (uint32_t)(keep < fs->ndzones ? keep : fs->ndzones);
	    i < fs->ndzones; i++) {
		if ((r = drop_zone(fs, ip->zone[i])) < 0)
			return r;
		ip->zone[i] = 0;
	}
	base = fs->ndzones;
	per = 1;
	for (level = 1; level <= fs->nlevels; level++) {
		per *= fs->nindirs;
		slot = fs->ndzones + level - 1;
		if (ip->zone[slot] != 0 &&
		    (r = trunc_indirect(fs, &ip->zone[slot], level,
		    keep > base ? keep - base : 0)) < 0)
			return r;
		base += per;
	}
	return 0;
}

/*
 * Clear the bytes of the file *ip from byte from to the end of its zone,
 * so that growing the file later shows zeros there.
 */
static int
zero_tail(struct mfs *fs, struct mfs_inode *ip, uint32_t from)
{
	uint64_t zbytes;
	uint32_t block, in, n, zone;
	int r;

	zbytes = (uint64_t)fs->block_size << fs->log_zone_size;
	if (from % zbytes == 0)
		return 0;
	if ((r = file_zone(fs, ip, from / zbytes, 0, &zone)) < 0 || zone == 0)
		return r;
	in = (uint32_t)(from % zbytes);
	for (n = in / fs->block_size; n < 1U << fs->log_zone_size; n++) {
		block = (zone << fs->log_zone_size) + n;
		if ((r = mfs_read_block(fs, block, fs->dbuf)) < 0)
			return r;
		if (n == in / fs->block_size)
			(void)memset(fs->dbuf + in % fs->block_size, 0,
			    fs->block_size - in % fs->block_size);
		else
			(void)memset(fs->dbuf, 0, fs->block_size);
		if ((r = mfs_write_block(fs, block, fs->dbuf)) < 0)
			return r;
	}
	return 0;
}

/*
 * Leave nothing of the file *ip past byte size: free the zones after the
 * one that holds it, and clear the rest of that one.  A file grows over
 * zeros then, whatever was there: data of a write that failed, or zones
 * past the end that another system left.  The clearing goes first, so
 * that a zone it cannot reach leaves everything as it was.
 */
static int
cut_back(struct mfs *fs, struct mfs_inode *ip, uint32_t size)
{
	uint64_t zbytes;
	int r;

	zbytes = (uint64_t)fs->block_size << fs->log_zone_size;
	if ((r = zero_tail(fs, ip, size)) < 0)
		return r;
	return free_from(fs, ip, (size + zbytes - 1) / zbytes);
}

int
mfs_resize(struct mfs *fs, struct mfs_inode *ip, uint32_t size)
{
	int r;

	if ((r = load_maps(fs)) < 0)
		return r;
	if (mfs_is_dev(ip))
		return -EINVAL;
	if (size > mfs_max_write(fs))
		return -EFBIG;
	if (size != ip->size &&
	    (r = cut_back(fs, ip, size < ip->size ? size : ip->size)) < 0)
		return r;
	ip->size = size;
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
	uint32_t block, in, n, old, zone;
	int r;

	if ((r = load_maps(fs)) < 0)
		return r;
	end = (uint64_t)off + len;
	if (end > mfs_max_write(fs) || end > UINT32_MAX)
		return -EFBIG;
	old = ip->size;
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
			goto fail;
		if (zone == 0)
			continue;
		block = (zone << fs->log_zone_size) +
		    (block & ((1U << fs->log_zone_size) - 1));
		if (n < fs->block_size &&
		    (r = mfs_read_block(fs, block, fs->dbuf)) < 0)
			goto fail;
		(void)memcpy(fs->dbuf + in, p, n);
		if ((r = mfs_write_block(fs, block, fs->dbuf)) < 0)
			goto fail;
	}
	if (end > ip->size)
		ip->size = (uint32_t)end;
	return 0;
fail:
	/* What went past the end goes, with the zones taken for it. */
	if (end > old)
		(void)cut_back(fs, ip, old);
	return r;
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

/*
 * Add an entry to a flex directory, as search_dir() of Minix-vmd enters
 * one: each block is walked from entry to entry, and the entry goes where
 * enough free slots follow one another in a block, or else at the start
 * of a new block.  The name is padded with NULs to the end of its last
 * slot.
 */
static int
add_flex(struct mfs *fs, struct mfs_inode *dp, const char *name,
    uint32_t ino)
{
	unsigned char entry[FLEX_SLOT * (FLEX_MAX_EXTENT + 1)];
	uint32_t nblocks, off, pos, slots, s, need, nfree;
	unsigned char *buf, *p;
	ssize_t n;
	size_t len;

	len = strlen(name);
	need = 1 + (uint32_t)(len + 3) / FLEX_SLOT;
	slots = fs->block_size / FLEX_SLOT;
	nblocks = (dp->size + fs->block_size - 1) / fs->block_size;
	/* mfs_pread() reads through fs->dbuf. */
	if ((buf = malloc(fs->block_size)) == NULL)
		return -ENOMEM;
	off = nblocks * fs->block_size;
	for (pos = 0; pos < nblocks; pos++) {
		/* What lies past the size reads as free slots, as zeros. */
		(void)memset(buf, 0, fs->block_size);
		if ((n = mfs_pread(fs, dp, buf, fs->block_size,
		    pos * fs->block_size)) < 0) {
			free(buf);
			return (int)n;
		}
		nfree = 0;
		for (s = 0; s < slots; s += 1 + p[FLEX_EXTENT]) {
			p = buf + s * FLEX_SLOT;
			if (load16(fs->order, p) != 0)
				nfree = 0;
			else if (++nfree == need)
				break;
		}
		if (nfree == need) {
			off = pos * fs->block_size + (s + 1 - need) * FLEX_SLOT;
			break;
		}
	}
	free(buf);
	(void)memset(entry, 0, sizeof(entry));
	put16(fs->order, entry, ino);
	entry[FLEX_EXTENT] = (unsigned char)(need - 1);
	(void)memcpy(entry + FLEX_NAME, name, len);
	return mfs_pwrite(fs, dp, entry, need * FLEX_SLOT, off);
}

int
mfs_add_entry(struct mfs *fs, struct mfs_inode *dp, const char *name,
    uint32_t ino)
{
	unsigned char entry[4 + MFS_MAX_NAME];
	struct free_slot s;
	size_t len;
	int r;

	if ((len = strlen(name)) > fs->namelen)
		return -ENAMETOOLONG;
	if (fs->flex)
		return add_flex(fs, dp, name, ino);
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
