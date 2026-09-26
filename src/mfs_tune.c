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
#include <unistd.h>

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

/* The maps of V3 are bytes in both byte orders; see mfs_map_bit(). */
static void
swap_map(const struct mfs *fs, unsigned char *buf)
{
	if (fs->version != 3)
		swap_words(buf, fs->block_size, 2);
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
	/* The super block and flex directories of Minix-vmd differ. */
	if (fs->vmd)
		return -ENOTSUP;
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

/*
 * Changing the name length.
 */

/* One directory, as it was read before anything is written. */
struct dir_copy {
	struct mfs_dirent *ent;		/* the entries in use, in order */
	uint32_t	*zones;		/* every zone it had */
	size_t		nent;
	size_t		maxent;
	size_t		nzones;
	size_t		maxzones;
	uint32_t	ino;
	uint32_t	need;		/* zones it needs with new entries */
};

struct rename_state {
	struct mfs	*fs;
	struct dir_copy	*dirs;
	size_t		ndirs;
	uint32_t	namelen;	/* the new one */
	int		error;
};

static int
copy_entry(const struct mfs_dirent *de, void *arg)
{
	struct dir_copy *d;
	struct mfs_dirent *p;

	d = arg;
	if (d->nent == d->maxent) {
		d->maxent = d->maxent == 0 ? 16 : d->maxent * 2;
		if ((p = realloc(d->ent, d->maxent * sizeof(*p))) == NULL)
			return -ENOMEM;
		d->ent = p;
	}
	d->ent[d->nent++] = *de;
	return 0;
}

static int
copy_zone(uint32_t zone, int level, const struct mfs_zref *ref, void *arg)
{
	struct rename_state *rs;
	struct dir_copy *d;
	uint32_t *p;

	(void)level;
	(void)ref;
	rs = arg;
	d = &rs->dirs[rs->ndirs - 1];
	if (zone < rs->fs->firstdatazone || zone >= rs->fs->nzones)
		return MFS_WALK_SKIP;
	if (d->nzones == d->maxzones) {
		d->maxzones = d->maxzones == 0 ? 16 : d->maxzones * 2;
		if ((p = realloc(d->zones, d->maxzones * sizeof(*p))) == NULL)
			return -ENOMEM;
		d->zones = p;
	}
	d->zones[d->nzones++] = zone;
	return 0;
}

/*
 * The zones a directory of n entries of size bytes takes: data zones
 * and the indirect zones that list them.  0 if the double indirect zone
 * cannot reach so far.
 */
static uint32_t
zones_for(const struct mfs *fs, size_t n, uint32_t size)
{
	uint64_t bytes, data, zbytes, rest;

	zbytes = (uint64_t)fs->block_size << fs->log_zone_size;
	bytes = (uint64_t)n * size;
	data = (bytes + zbytes - 1) / zbytes;
	if (data <= fs->ndzones)
		return (uint32_t)data;
	rest = data - fs->ndzones;
	if (rest <= fs->nindirs)
		return (uint32_t)(data + 1);
	rest -= fs->nindirs;
	if (rest > (uint64_t)fs->nindirs * fs->nindirs)
		return 0;
	return (uint32_t)(data + 2 + (rest + fs->nindirs - 1) / fs->nindirs);
}

/* Read every directory, and check that the new entries fit. */
static int
read_dirs(struct rename_state *rs)
{
	struct mfs_inode ip;
	struct dir_copy *d;
	struct mfs *fs;
	uint32_t ino, size;
	size_t i;
	int r;

	fs = rs->fs;
	size = fs->dirent_ino + rs->namelen;
	for (ino = 1; ino <= fs->ninodes; ino++) {
		if ((r = mfs_get_inode(fs, ino, &ip)) < 0)
			return r;
		if (ip.mode == 0 || !mfs_is_dir(&ip))
			continue;
		d = realloc(rs->dirs, (rs->ndirs + 1) * sizeof(*d));
		if (d == NULL)
			return -ENOMEM;
		rs->dirs = d;
		d = &rs->dirs[rs->ndirs++];
		(void)memset(d, 0, sizeof(*d));
		d->ino = ino;
		if ((r = mfs_readdir(fs, &ip, copy_entry, d)) < 0 ||
		    (r = mfs_walk_zones(fs, &ip, copy_zone, rs)) < 0)
			return r;
		for (i = 0; i < d->nent; i++)
			if (strlen(d->ent[i].name) > rs->namelen)
				return -ENAMETOOLONG;
		if ((d->need = zones_for(fs, d->nent, size)) == 0 &&
		    d->nent > 0)
			return -EFBIG;
	}
	return 0;
}

/* Take the first free zone of the map. */
static uint32_t
take_zone(struct mfs *fs, unsigned char *zmap, uint32_t *next)
{
	uint32_t n;

	n = fs->nzones - fs->firstdatazone;
	for (; *next <= n; (*next)++) {
		if (!mfs_map_bit(fs, zmap, *next)) {
			mfs_set_map_bit(fs, zmap, *next, 1);
			return fs->firstdatazone + (*next)++ - 1;
		}
	}
	return 0;
}

/* Take a free zone for an indirect zone, and fill it with zeros. */
static uint32_t
take_indirect(struct mfs *fs, unsigned char *zmap, uint32_t *next,
    unsigned char *zero)
{
	uint32_t i, z;

	if ((z = take_zone(fs, zmap, next)) == 0)
		return 0;
	for (i = 0; i < 1U << fs->log_zone_size; i++)
		if (mfs_write_block(fs, (z << fs->log_zone_size) + i, zero) < 0)
			return 0;
	return z;
}

/* Set entry index of the indirect zone ind to zone. */
static int
set_listed(struct mfs *fs, uint32_t ind, uint32_t index, uint32_t zone)
{
	struct mfs_zref ref;

	ref.block = ind << fs->log_zone_size;
	ref.index = index;
	return mfs_set_zref(fs, NULL, &ref, zone);
}

/* Write a whole zone from buf, which holds a zone of bytes. */
static int
write_zone(struct mfs *fs, uint32_t zone, const unsigned char *buf)
{
	uint32_t i;
	int r;

	for (i = 0; i < 1U << fs->log_zone_size; i++) {
		r = mfs_write_block(fs, (zone << fs->log_zone_size) + i,
		    buf + (size_t)i * fs->block_size);
		if (r < 0)
			return r;
	}
	return 0;
}

/*
 * Write directory d with entries of the new size into zones taken from
 * zmap, and point its inode at them.
 */
static int
write_dir(struct rename_state *rs, const struct dir_copy *d,
    unsigned char *zmap, uint32_t *next, unsigned char *buf)
{
	struct mfs_inode ip;
	struct mfs *fs;
	unsigned char *p, *zero;
	uint64_t zbytes;
	uint32_t dsize, k, ndata, per, z;
	uint32_t ind1, ind2, sub;
	size_t e, len;
	int r;

	fs = rs->fs;
	zbytes = (uint64_t)fs->block_size << fs->log_zone_size;
	dsize = fs->dirent_ino + rs->namelen;
	per = (uint32_t)(zbytes / dsize);
	if ((r = mfs_get_inode(fs, d->ino, &ip)) < 0)
		return r;
	(void)memset(ip.zone, 0, sizeof(ip.zone));
	ndata = (uint32_t)((d->nent + per - 1) / per);
	/* The last block of buf is kept for zeros. */
	zero = buf + zbytes;
	ind1 = ind2 = sub = 0;
	for (k = 0, e = 0; k < ndata; k++) {
		(void)memset(buf, 0, (size_t)zbytes);
		for (p = buf; e < d->nent && p + dsize <= buf + zbytes;
		    e++, p += dsize) {
			if (fs->dirent_ino == 2)
				put16(fs->order, p, d->ent[e].ino);
			else
				put32(fs->order, p, d->ent[e].ino);
			len = strlen(d->ent[e].name);
			(void)memcpy(p + fs->dirent_ino, d->ent[e].name, len);
		}
		if ((z = take_zone(fs, zmap, next)) == 0)
			return -ENOSPC;
		if ((r = write_zone(fs, z, buf)) < 0)
			return r;

		/* Where zone k of the directory is listed. */
		if (k < fs->ndzones) {
			ip.zone[k] = z;
		} else if (k - fs->ndzones < fs->nindirs) {
			if (ind1 == 0) {
				if ((ind1 = take_indirect(fs, zmap, next,
				    zero)) == 0)
					return -ENOSPC;
				ip.zone[fs->ndzones] = ind1;
			}
			r = set_listed(fs, ind1, k - fs->ndzones, z);
		} else {
			if (ind2 == 0) {
				if ((ind2 = take_indirect(fs, zmap, next,
				    zero)) == 0)
					return -ENOSPC;
				ip.zone[fs->ndzones + 1] = ind2;
			}
			if ((k - fs->ndzones - fs->nindirs) % fs->nindirs ==
			    0) {
				if ((sub = take_indirect(fs, zmap, next,
				    zero)) == 0)
					return -ENOSPC;
				r = set_listed(fs, ind2, (k - fs->ndzones -
				    fs->nindirs) / fs->nindirs, sub);
				if (r < 0)
					return r;
			}
			r = set_listed(fs, sub, (k - fs->ndzones -
			    fs->nindirs) % fs->nindirs, z);
		}
		if (r < 0)
			return r;
	}
	ip.size = (uint32_t)(d->nent * dsize);
	return mfs_put_inode(fs, &ip);
}

int
mfs_change_namelen(struct mfs *fs, uint32_t namelen, int check)
{
	struct rename_state rs;
	unsigned char sb[SUPER_SIZE], *buf, *zmap;
	uint64_t avail, need;
	uint32_t bit, n, next;
	uint16_t magic;
	size_t i, j;
	int r;

	if (!fs->writable && !check)
		return -EROFS;
	if (fs->version == 3 || (namelen != 14 && namelen != 30))
		return -EINVAL;
	if (fs->vmd)
		return -ENOTSUP;
	if (fs->namelen == namelen)
		return 0;
	(void)memset(&rs, 0, sizeof(rs));
	rs.fs = fs;
	rs.namelen = namelen;
	zmap = NULL;
	/* A zone of data, and a block of zeros after it. */
	buf = calloc(((size_t)1 << fs->log_zone_size) + 1, fs->block_size);
	if (buf == NULL) {
		r = -ENOMEM;
		goto out;
	}
	if ((r = read_dirs(&rs)) < 0 ||
	    (r = mfs_load_map(fs, MFS_ZMAP, &zmap)) < 0)
		goto out;

	/* The directories give back their zones and take new ones. */
	n = fs->nzones - fs->firstdatazone;
	avail = need = 0;
	for (bit = 1; bit <= n; bit++)
		if (!mfs_map_bit(fs, zmap, bit))
			avail++;
	for (i = 0; i < rs.ndirs; i++) {
		avail += rs.dirs[i].nzones;
		need += rs.dirs[i].need;
	}
	if (need > avail) {
		r = -ENOSPC;
		goto out;
	}
	if (check)
		goto out;

	for (i = 0; i < rs.ndirs && r == 0; i++) {
		for (j = 0; j < rs.dirs[i].nzones; j++)
			mfs_set_map_bit(fs, zmap,
			    rs.dirs[i].zones[j] - fs->firstdatazone + 1, 0);
		next = 1;
		r = write_dir(&rs, &rs.dirs[i], zmap, &next, buf);
	}
	if (r == 0)
		r = mfs_store_map(fs, MFS_ZMAP, zmap);
	if (r == 0)
		r = mfs_read_device(fs, sb, sizeof(sb), SUPER_OFFSET);
	if (r == 0) {
		if (fs->version == 1)
			magic = namelen == 14 ? MFS_MAGIC_V1 : MFS_MAGIC_V1L;
		else
			magic = namelen == 14 ? MFS_MAGIC_V2 : MFS_MAGIC_V2L;
		put16(fs->order, sb + SB12_MAGIC, magic);
		r = mfs_write_device(fs, sb, sizeof(sb), SUPER_OFFSET);
	}
	if (r == 0) {
		fs->magic = magic;
		fs->namelen = namelen;
		fs->dirent_size = fs->dirent_ino + namelen;
	}
out:
	for (i = 0; i < rs.ndirs; i++) {
		free(rs.dirs[i].ent);
		free(rs.dirs[i].zones);
	}
	free(rs.dirs);
	free(zmap);
	free(buf);
	return r;
}

/*
 * Growing.
 */

#define MAX_16		0xffff

/* The zones to move and the numbers to change, found before writing. */
struct grow {
	struct mfs	*fs;
	unsigned char	*indirect;	/* bit per zone: an indirect zone */
};

static int
note_grow(uint32_t zone, int level, const struct mfs_zref *ref, void *arg)
{
	struct grow *g;

	(void)ref;
	g = arg;
	if (zone < g->fs->firstdatazone || zone >= g->fs->nzones)
		return MFS_WALK_SKIP;
	if (level > 0)
		set_bit(g->indirect, zone);
	return 0;
}

/* Move n blocks from block from to block to, which is higher. */
static int
move_blocks(struct mfs *fs, uint32_t from, uint32_t to, uint32_t n,
    unsigned char *buf)
{
	uint32_t i;
	int r;

	for (i = n; i-- > 0; ) {
		if ((r = mfs_read_block(fs, from + i, buf)) < 0 ||
		    (r = mfs_write_block(fs, to + i, buf)) < 0)
			return r;
	}
	return 0;
}

/* Add delta to a zone number of the old data area, and leave others. */
static uint32_t
moved(uint32_t zone, uint32_t first, uint32_t nzones, uint32_t delta)
{
	if (zone < first || zone >= nzones)
		return zone;
	return zone + delta;
}

static void
put_zone(const struct mfs *fs, unsigned char *p, uint32_t zone)
{
	if (fs->zone_num_size == 2)
		put16(fs->order, p, zone);
	else
		put32(fs->order, p, zone);
}

static uint32_t
get_zone(const struct mfs *fs, const unsigned char *p)
{
	if (fs->zone_num_size == 2 && fs->order == MFS_BIG_ENDIAN)
		return (uint32_t)(p[0] << 8 | p[1]);
	if (fs->zone_num_size == 2)
		return (uint32_t)(p[1] << 8 | p[0]);
	if (fs->order == MFS_BIG_ENDIAN)
		return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 |
		    (uint32_t)p[2] << 8 | p[3];
	return (uint32_t)p[3] << 24 | (uint32_t)p[2] << 16 |
	    (uint32_t)p[1] << 8 | p[0];
}

/* Write the new size, zone map blocks and first data zone. */
static int
grow_super(struct mfs *fs, uint32_t nzones, uint32_t zmap_blocks,
    uint32_t first)
{
	unsigned char sb[SUPER_SIZE];
	int r;

	if ((r = mfs_read_device(fs, sb, sizeof(sb), SUPER_OFFSET)) < 0)
		return r;
	if (fs->version == 3) {
		put32(fs->order, sb + SB3_ZONES, nzones);
		put16(fs->order, sb + SB3_ZMAP, zmap_blocks);
		put16(fs->order, sb + SB3_FIRSTDATA, first <= MAX_16 ? first :
		    0);
	} else {
		if (fs->version == 1)
			put16(fs->order, sb + SB12_NZONES, nzones);
		else
			put32(fs->order, sb + SB12_ZONES, nzones);
		put16(fs->order, sb + SB12_ZMAP, zmap_blocks);
		put16(fs->order, sb + SB12_FIRSTDATA, first);
	}
	return mfs_write_device(fs, sb, sizeof(sb), SUPER_OFFSET);
}

int
mfs_grow(struct mfs *fs, uint32_t nblocks, int flags)
{
	struct mfs_inode ip;
	struct grow g;
	unsigned char *buf, *oldmap, *zmap;
	uint64_t bits, oldbits, need, newbits, size;
	uint32_t delta, first, grow_blocks, i, ino, itable, k, newz, z;
	uint32_t oldfirst, oldz;
	int pad, r, resize;

	if (!fs->writable && (flags & MFS_RESIZE_CHECK) == 0)
		return -EROFS;
	if (fs->tracks.size != 0)
		return -EINVAL;
	newz = nblocks >> fs->log_zone_size;
	if (newz < fs->nzones)
		return -EINVAL;
	if (newz == fs->nzones)
		return 0;
	if (fs->version == 1 && newz > MAX_16)
		return -EFBIG;
	/* A device, or an image kept as it is, has to hold the zones. */
	size = ((uint64_t)newz << fs->log_zone_size) * fs->block_size;
	resize = fs->regular && (flags & MFS_RESIZE_KEEP) == 0;
	if (!resize && (fs->image_size < 0 ||
	    (uint64_t)fs->image_size < size))
		return -ENXIO;

	/* The zone map needs a bit for each data zone, and bit 0. */
	bits = (uint64_t)fs->block_size * 8;
	oldfirst = fs->firstdatazone;
	oldz = fs->nzones;
	itable = (uint32_t)(((uint64_t)fs->ninodes * fs->inode_size +
	    fs->block_size - 1) / fs->block_size);
	k = fs->zmap_blocks;
	need = ((uint64_t)newz - oldfirst + 1 + bits - 1) / bits;
	first = oldfirst;
	if (need > k) {
		if (need > MAX_16)
			return -EFBIG;
		k = (uint32_t)need;
		z = (START_BLOCK + fs->imap_blocks + k + itable +
		    (1U << fs->log_zone_size) - 1) >> fs->log_zone_size;
		if (z > first)
			first = z;
	}
	delta = first - oldfirst;
	if (fs->version != 3 && first > MAX_16)
		return -EFBIG;
	/* The zones that move up must fit. */
	if (oldz + delta > newz)
		return -ENOSPC;
	grow_blocks = (k - fs->zmap_blocks);

	(void)memset(&g, 0, sizeof(g));
	g.fs = fs;
	oldmap = zmap = NULL;
	buf = malloc(fs->block_size);
	g.indirect = calloc((size_t)oldz / 8 + 1, 1);
	if (buf == NULL || g.indirect == NULL) {
		r = -ENOMEM;
		goto out;
	}

	/* Read: the indirect zones, and the zone map. */
	for (ino = 1; ino <= fs->ninodes && delta > 0; ino++) {
		if ((r = mfs_get_inode(fs, ino, &ip)) < 0)
			goto out;
		if (ip.mode != 0 && !mfs_is_dev(&ip) &&
		    (r = mfs_walk_zones(fs, &ip, note_grow, &g)) < 0)
			goto out;
	}
	if ((r = mfs_load_map(fs, MFS_ZMAP, &oldmap)) < 0)
		goto out;
	if ((zmap = calloc(k, fs->block_size)) == NULL) {
		r = -ENOMEM;
		goto out;
	}
	(void)memcpy(zmap, oldmap, (size_t)fs->zmap_blocks * fs->block_size);
	oldbits = (uint64_t)oldz - oldfirst;
	newbits = (uint64_t)newz - first;
	/*
	 * The bits past the end follow the old ones, or else are what a new
	 * file system gets.
	 */
	pad = oldbits + 1 < (uint64_t)fs->zmap_blocks * bits ?
	    mfs_map_bit(fs, oldmap, (uint32_t)oldbits + 1) :
	    mfs_default_map_end(fs->version, fs->namelen);
	for (i = (uint32_t)oldbits + 1; i < k * bits; i++)
		mfs_set_map_bit(fs, zmap, i, i > newbits ? pad : 0);
	if (flags & MFS_RESIZE_CHECK)
		goto out;

	/* Write: the file grows, then the zones and the inode table move. */
	if (resize) {
		if (ftruncate(fs->fd, (off_t)size) == -1) {
			r = -errno;
			goto out;
		}
		fs->file_size = fs->image_size = (off_t)size;
	}
	fs->nblocks = newz << fs->log_zone_size;
	for (z = oldz; delta > 0 && z-- > oldfirst; ) {
		if (!mfs_map_bit(fs, oldmap, z - oldfirst + 1))
			continue;
		r = move_blocks(fs, z << fs->log_zone_size,
		    (z + delta) << fs->log_zone_size,
		    1U << fs->log_zone_size, buf);
		if (r < 0)
			goto out;
	}
	if (grow_blocks > 0 && (r = move_blocks(fs, fs->inode_start,
	    fs->inode_start + grow_blocks, itable, buf)) < 0)
		goto out;
	fs->inode_start += grow_blocks;
	fs->zmap_blocks = k;

	/* The zone numbers in inodes and indirect zones follow. */
	for (ino = 1; ino <= fs->ninodes && delta > 0; ino++) {
		if ((r = mfs_get_inode(fs, ino, &ip)) < 0)
			goto out;
		if (ip.mode == 0 || mfs_is_dev(&ip))
			continue;
		for (i = 0; i < MFS_NR_ZONES; i++)
			ip.zone[i] = moved(ip.zone[i], oldfirst, oldz, delta);
		if ((r = mfs_put_inode(fs, &ip)) < 0)
			goto out;
	}
	for (z = oldfirst; z < oldz && delta > 0; z++) {
		if (!test_bit(g.indirect, z))
			continue;
		if ((r = mfs_read_block(fs, (z + delta) << fs->log_zone_size,
		    buf)) < 0)
			goto out;
		for (i = 0; i < fs->nindirs; i++)
			put_zone(fs, buf + i * fs->zone_num_size,
			    moved(get_zone(fs, buf + i * fs->zone_num_size),
			    oldfirst, oldz, delta));
		if ((r = mfs_write_block(fs, (z + delta) << fs->log_zone_size,
		    buf)) < 0)
			goto out;
	}
	if ((r = mfs_store_map(fs, MFS_ZMAP, zmap)) < 0 ||
	    (r = grow_super(fs, newz, k, first)) < 0)
		goto out;
	fs->nzones = newz;
	fs->firstdatazone = first;
out:
	free(g.indirect);
	free(oldmap);
	free(zmap);
	free(buf);
	return r;
}

/*
 * Shrinking.
 */

/* A zone number past the new end, and where it is kept. */
struct tail_ref {
	struct mfs_zref	ref;		/* block 0: slot of inode ino */
	uint32_t	ino;
	uint32_t	zone;
};

struct shrink {
	struct mfs	*fs;
	struct tail_ref	*refs;
	size_t		n;
	size_t		max;
	uint32_t	newz;
	uint32_t	ino;
};

static int
note_tail(uint32_t zone, int level, const struct mfs_zref *ref, void *arg)
{
	struct shrink *s;
	struct tail_ref *t;

	(void)level;
	s = arg;
	if (zone < s->fs->firstdatazone || zone >= s->fs->nzones)
		return MFS_WALK_SKIP;
	if (zone < s->newz)
		return 0;
	if (s->n == s->max) {
		s->max = s->max == 0 ? 64 : s->max * 2;
		if ((t = realloc(s->refs, s->max * sizeof(*t))) == NULL)
			return -ENOMEM;
		s->refs = t;
	}
	t = &s->refs[s->n++];
	t->ref = *ref;
	t->ino = s->ino;
	t->zone = zone;
	return 0;
}

int
mfs_shrink(struct mfs *fs, uint32_t nblocks, int flags)
{
	struct mfs_inode ip;
	struct shrink s;
	struct mfs_zref at;
	unsigned char *buf, *zmap;
	uint32_t *newloc, bit, cursor, i, ino, n, ntail, z, zb;
	int pad, r;

	if (!fs->writable && (flags & MFS_RESIZE_CHECK) == 0)
		return -EROFS;
	if (fs->tracks.size != 0)
		return -EINVAL;
	(void)memset(&s, 0, sizeof(s));
	s.fs = fs;
	s.newz = nblocks >> fs->log_zone_size;
	if (s.newz >= fs->nzones)
		return -EINVAL;
	if (s.newz <= fs->firstdatazone)
		return -ENOSPC;
	zb = 1U << fs->log_zone_size;
	ntail = fs->nzones - s.newz;
	zmap = NULL;
	buf = malloc(fs->block_size);
	newloc = calloc(ntail, sizeof(*newloc));
	if (buf == NULL || newloc == NULL) {
		r = -ENOMEM;
		goto out;
	}

	/* Read: every zone number past the new end, and the zone map. */
	for (ino = 1; ino <= fs->ninodes; ino++) {
		if ((r = mfs_get_inode(fs, ino, &ip)) < 0)
			goto out;
		if (ip.mode == 0 || mfs_is_dev(&ip))
			continue;
		s.ino = ino;
		if ((r = mfs_walk_zones(fs, &ip, note_tail, &s)) < 0)
			goto out;
	}
	if ((r = mfs_load_map(fs, MFS_ZMAP, &zmap)) < 0)
		goto out;

	/* Give each of those zones a free one below the new end. */
	n = s.newz - fs->firstdatazone;
	cursor = 1;
	for (i = 0; i < s.n; i++) {
		z = s.refs[i].zone - s.newz;
		if (newloc[z] != 0)
			continue;
		while (cursor <= n && mfs_map_bit(fs, zmap, cursor))
			cursor++;
		if (cursor > n) {
			r = -ENOSPC;
			goto out;
		}
		mfs_set_map_bit(fs, zmap, cursor, 1);
		newloc[z] = fs->firstdatazone + cursor - 1;
	}
	if (flags & MFS_RESIZE_CHECK)
		goto out;

	/* Write: the zones move down, then the numbers follow them. */
	for (z = 0; z < ntail; z++) {
		if (newloc[z] == 0)
			continue;
		for (i = 0; i < zb; i++) {
			if ((r = mfs_read_block(fs, ((s.newz + z) <<
			    fs->log_zone_size) + i, buf)) < 0 ||
			    (r = mfs_write_block(fs, (newloc[z] <<
			    fs->log_zone_size) + i, buf)) < 0)
				goto out;
		}
	}
	for (i = 0; i < s.n; i++) {
		z = newloc[s.refs[i].zone - s.newz];
		if (s.refs[i].ref.block == 0) {
			if ((r = mfs_get_inode(fs, s.refs[i].ino, &ip)) < 0)
				goto out;
			ip.zone[s.refs[i].ref.index] = z;
			if ((r = mfs_put_inode(fs, &ip)) < 0)
				goto out;
			continue;
		}
		/* The indirect zone that lists it may have moved too. */
		at = s.refs[i].ref;
		bit = at.block >> fs->log_zone_size;
		if (bit >= s.newz)
			at.block = newloc[bit - s.newz] << fs->log_zone_size;
		if ((r = mfs_set_zref(fs, NULL, &at, z)) < 0)
			goto out;
	}

	/*
	 * The bits past the new end follow the old ones, or else are what a
	 * new file system gets.
	 */
	n = fs->nzones - fs->firstdatazone;
	pad = (uint64_t)n + 1 < (uint64_t)fs->zmap_blocks *
	    fs->block_size * 8 ? mfs_map_bit(fs, zmap, n + 1) :
	    mfs_default_map_end(fs->version, fs->namelen);
	for (bit = s.newz - fs->firstdatazone + 1; bit <= n; bit++)
		mfs_set_map_bit(fs, zmap, bit, pad);
	if ((r = mfs_store_map(fs, MFS_ZMAP, zmap)) < 0 ||
	    (r = grow_super(fs, s.newz, fs->zmap_blocks,
	    fs->firstdatazone)) < 0)
		goto out;
	fs->nzones = s.newz;
	fs->nblocks = s.newz << fs->log_zone_size;
	if (fs->regular && (flags & MFS_RESIZE_KEEP) == 0) {
		if (ftruncate(fs->fd, (off_t)fs->nblocks * fs->block_size) ==
		    -1) {
			r = -errno;
			goto out;
		}
		fs->file_size = fs->image_size =
		    (off_t)fs->nblocks * fs->block_size;
	}
out:
	free(s.refs);
	free(newloc);
	free(zmap);
	free(buf);
	return r;
}
