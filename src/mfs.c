/*
 * SPDX-License-Identifier: BSD-2-Clause
 * Copyright (c) 2026 Shoichi Fukusaka
 *
 * mfs.c - read access to MINIX file system images.
 *
 * The layout is described in layout.h.
 */

#include <sys/stat.h>

#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "layout.h"
#include "mfs.h"

static uint16_t
get16(const struct mfs *fs, const unsigned char *p)
{
	if (fs->order == MFS_BIG_ENDIAN)
		return (uint16_t)(p[0] << 8 | p[1]);
	return (uint16_t)(p[1] << 8 | p[0]);
}

static uint32_t
get32(const struct mfs *fs, const unsigned char *p)
{
	if (fs->order == MFS_BIG_ENDIAN)
		return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 |
		    (uint32_t)p[2] << 8 | p[3];
	return (uint32_t)p[3] << 24 | (uint32_t)p[2] << 16 |
	    (uint32_t)p[1] << 8 | p[0];
}

/* A number of 2 or 4 bytes, such as a zone or inode number. */
static uint32_t
getn(const struct mfs *fs, const unsigned char *p, uint32_t size)
{
	if (size == 2)
		return get16(fs, p);
	return get32(fs, p);
}

/* Read exactly len bytes at off, or fail with -EIO. */
static int
read_at(int fd, void *buf, size_t len, off_t off)
{
	unsigned char *p;
	ssize_t n;

	p = buf;
	while (len > 0) {
		n = pread(fd, p, len, off);
		if (n == -1) {
			if (errno == EINTR)
				continue;
			return -errno;
		}
		if (n == 0)
			return -EIO;
		p += n;
		len -= (size_t)n;
		off += n;
	}
	return 0;
}

/*
 * Recognise the magic number and set the version and byte order.
 * Returns 0, or -EINVAL if this is not a MINIX file system.
 */
static int
recognise(struct mfs *fs, const unsigned char *sb)
{
	static const struct {
		uint16_t	magic;
		int		version;
		int		offset;
	} known[] = {
		{ MFS_MAGIC_V1,  1, SB12_MAGIC },
		{ MFS_MAGIC_V1L, 1, SB12_MAGIC },
		{ MFS_MAGIC_V2,  2, SB12_MAGIC },
		{ MFS_MAGIC_V2L, 2, SB12_MAGIC },
		{ MFS_MAGIC_V3,  3, SB3_MAGIC }
	};
	const unsigned char *p;
	size_t i;

	for (i = 0; i < sizeof(known) / sizeof(known[0]); i++) {
		p = sb + known[i].offset;
		if ((p[1] << 8 | p[0]) == known[i].magic)
			fs->order = MFS_LITTLE_ENDIAN;
		else if ((p[0] << 8 | p[1]) == known[i].magic)
			fs->order = MFS_BIG_ENDIAN;
		else
			continue;
		fs->magic = known[i].magic;
		fs->version = known[i].version;
		return 0;
	}
	return -EINVAL;
}

/* Fill in the fields of the super block and those that follow from them. */
static void
read_super(struct mfs *fs, const unsigned char *sb)
{
	if (fs->version == 3) {
		fs->ninodes = get32(fs, sb + SB3_NINODES);
		fs->imap_blocks = get16(fs, sb + SB3_IMAP);
		fs->zmap_blocks = get16(fs, sb + SB3_ZMAP);
		fs->firstdatazone = get16(fs, sb + SB3_FIRSTDATA);
		fs->log_zone_size = get16(fs, sb + SB3_LOGZONE);
		fs->max_size = get32(fs, sb + SB3_MAXSIZE);
		fs->nzones = get32(fs, sb + SB3_ZONES);
		fs->block_size = get16(fs, sb + SB3_BLOCKSIZE);
		fs->state = get16(fs, sb + SB3_FLAGS);
	} else {
		fs->ninodes = get16(fs, sb + SB12_NINODES);
		fs->imap_blocks = get16(fs, sb + SB12_IMAP);
		fs->zmap_blocks = get16(fs, sb + SB12_ZMAP);
		fs->firstdatazone = get16(fs, sb + SB12_FIRSTDATA);
		fs->log_zone_size = get16(fs, sb + SB12_LOGZONE);
		fs->max_size = get32(fs, sb + SB12_MAXSIZE);
		if (fs->version == 1)
			fs->nzones = get16(fs, sb + SB12_NZONES);
		else
			fs->nzones = get32(fs, sb + SB12_ZONES);
		fs->block_size = STATIC_BLOCK;
		fs->state = get16(fs, sb + SB12_STATE);
		if (sb[SBVMD_MAGIC] == SBVMD_MAGIC0 &&
		    sb[SBVMD_MAGIC + 1] == SBVMD_MAGIC1) {
			fs->vmd = 1;
			fs->log_zone_size = sb[SBVMD_LOGZONE];
			fs->state = sb[SBVMD_FLAGS];
			fs->flex = (fs->state & MFS_VMD_FLEX) != 0;
		}
	}

	switch (fs->magic) {
	case MFS_MAGIC_V1:
	case MFS_MAGIC_V2:
		fs->namelen = 14;
		break;
	case MFS_MAGIC_V1L:
	case MFS_MAGIC_V2L:
		fs->namelen = 30;
		break;
	default:
		fs->namelen = 60;
		break;
	}
	fs->dirent_ino = fs->version == 3 ? 4 : 2;
	fs->dirent_size = fs->dirent_ino + fs->namelen;
	if (fs->flex) {
		fs->namelen = FLEX_SLOT * (FLEX_MAX_EXTENT + 1) - FLEX_NAME - 1;
		fs->dirent_size = FLEX_SLOT;
	}
	fs->inode_size = fs->version == 1 ? I1_SIZE : I2_SIZE;
	fs->zone_num_size = fs->version == 1 ? 2 : 4;
	fs->ndzones = NR_DZONES;
	fs->nlevels = fs->version == 1 ? 2 : 3;
	fs->inode_start = START_BLOCK + fs->imap_blocks + fs->zmap_blocks;
}

/*
 * Bytes a file can have: the zones its slots reach, times the zone size.
 * The count saturates at 2^32 zones, beyond any 32-bit file size.
 */
static uint64_t
max_file_size(const struct mfs *fs)
{
	uint64_t per, zones;
	uint32_t i;

	zones = fs->ndzones;
	per = 1;
	for (i = 0; i < fs->nlevels; i++) {
		per *= fs->nindirs;
		zones += per;
		if (zones > UINT32_MAX)
			return (uint64_t)UINT32_MAX + 1;
	}
	return zones * ((uint64_t)fs->block_size << fs->log_zone_size);
}

/* Check that the super block describes a layout that makes sense. */
static int
check_super(struct mfs *fs)
{
	uint64_t data, first, itable, nblocks;

	if (fs->block_size < SUPER_SIZE || fs->block_size > MAX_BLOCK ||
	    fs->block_size % 512 != 0)
		return -EINVAL;
	if (fs->ninodes == 0 || fs->nzones == 0 || fs->imap_blocks == 0 ||
	    fs->zmap_blocks == 0 || fs->log_zone_size > MAX_LOG_ZONE)
		return -EINVAL;
	fs->nindirs = fs->block_size / fs->zone_num_size;
	itable = ((uint64_t)fs->ninodes * fs->inode_size + fs->block_size - 1)
	    / fs->block_size;

	/*
	 * V3 writes 0 when the first data zone does not fit in 16 bits; it
	 * then follows the inode table, as MINIX 3 computes it.
	 */
	if (fs->version == 3 && fs->firstdatazone == 0) {
		first = (fs->inode_start + itable +
		    ((uint64_t)1 << fs->log_zone_size) - 1) >>
		    fs->log_zone_size;
		if (first > UINT32_MAX)
			return -EINVAL;
		fs->firstdatazone = (uint32_t)first;
	}
	if (fs->firstdatazone >= fs->nzones)
		return -EINVAL;

	/* The bit maps must have a bit for every inode and zone. */
	if ((uint64_t)fs->imap_blocks * fs->block_size * 8 <
	    (uint64_t)fs->ninodes + 1)
		return -EINVAL;
	if ((uint64_t)fs->zmap_blocks * fs->block_size * 8 <
	    (uint64_t)fs->nzones - fs->firstdatazone + 1)
		return -EINVAL;

	data = (uint64_t)fs->firstdatazone << fs->log_zone_size;
	nblocks = (uint64_t)fs->nzones << fs->log_zone_size;
	if (fs->inode_start + itable > data || data > nblocks ||
	    nblocks > UINT32_MAX)
		return -EINVAL;
	fs->nblocks = (uint32_t)nblocks;
	fs->max_file = max_file_size(fs);
	return 0;
}

/*
 * Where byte off of the device is in the image file.  *len is cut to
 * what lies in one piece there.
 */
static off_t
file_offset(const struct mfs *fs, off_t off, size_t *len)
{
	uint64_t in, size, track;

	if (fs->tracks.size == 0)
		return off;
	size = fs->tracks.size;
	track = (uint64_t)off / size;
	in = (uint64_t)off % size;
	if (*len > size - in)
		*len = (size_t)(size - in);
	return (off_t)((track * fs->tracks.heads + fs->tracks.side) * size +
	    in);
}

/* Read len bytes of the device at off; see read_at(). */
static int
dev_read(struct mfs *fs, void *buf, size_t len, off_t off)
{
	unsigned char *p;
	off_t where;
	size_t n;
	int r;

	for (p = buf; len > 0; p += n, len -= n, off += (off_t)n) {
		n = len;
		where = file_offset(fs, off, &n);
		if ((r = read_at(fs->fd, p, n, where)) < 0)
			return r;
	}
	return 0;
}

/* Write len bytes of the device at off; see write_at(). */
static int
dev_write(struct mfs *fs, const void *buf, size_t len, off_t off)
{
	const unsigned char *p;
	off_t where;
	size_t n;
	int r;

	for (p = buf; len > 0; p += n, len -= n, off += (off_t)n) {
		n = len;
		where = file_offset(fs, off, &n);
		if ((r = write_at(fs->fd, p, n, where)) < 0)
			return r;
	}
	return 0;
}

/* The bytes of the device that the image file holds. */
static off_t
device_size(const struct mfs *fs)
{
	uint64_t cyl, rem, size;

	if (fs->tracks.size == 0)
		return fs->file_size;
	size = fs->tracks.size;
	cyl = size * fs->tracks.heads;
	rem = (uint64_t)fs->file_size % cyl;
	rem = rem > fs->tracks.side * size ? rem - fs->tracks.side * size : 0;
	return (off_t)((uint64_t)fs->file_size / cyl * size +
	    (rem < size ? rem : size));
}

uint32_t
mfs_minix_max_size(const struct mfs *fs)
{
	uint64_t max, zones;

	zones = fs->ndzones + fs->nindirs + (uint64_t)fs->nindirs * fs->nindirs;
	max = INT32_MAX;
	if (((max - 1) >> fs->log_zone_size) / fs->block_size >= zones)
		max = (zones * fs->block_size) << fs->log_zone_size;
	return (uint32_t)max;
}

int
mfs_read_device(struct mfs *fs, void *buf, size_t len, off_t off)
{
	return dev_read(fs, buf, len, off);
}

int
mfs_write_device(struct mfs *fs, const void *buf, size_t len, off_t off)
{
	if (!fs->writable)
		return -EROFS;
	return dev_write(fs, buf, len, off);
}

int
mfs_parse_tracks(const char *s, struct mfs_tracks *t)
{
	unsigned long v[3];
	char *end;
	int i;

	for (i = 0; i < 3; i++) {
		if (*s < '0' || *s > '9')
			return -EINVAL;
		errno = 0;
		v[i] = strtoul(s, &end, 10);
		if (errno != 0 || v[i] > UINT32_MAX ||
		    *end != (i < 2 ? ':' : '\0'))
			return -EINVAL;
		s = end + 1;
	}
	if (v[0] == 0 || v[2] >= v[1])
		return -EINVAL;
	t->size = (uint32_t)v[0];
	t->heads = (uint32_t)v[1];
	t->side = (uint32_t)v[2];
	return 0;
}

int
mfs_open_tracks(struct mfs *fs, const char *path, int rw,
    const struct mfs_tracks *tracks)
{
	unsigned char sb[SUPER_SIZE];
	struct stat st;
	int r;

	(void)memset(fs, 0, sizeof(*fs));
	if (tracks != NULL)
		fs->tracks = *tracks;
	if ((fs->fd = open(path, rw ? O_RDWR : O_RDONLY)) == -1)
		return -errno;
	fs->writable = rw;
	if (fstat(fs->fd, &st) == -1) {
		r = -errno;
		goto fail;
	}
	fs->file_size = st.st_size;
	fs->image_size = device_size(fs);
	if ((r = dev_read(fs, sb, sizeof(sb), SUPER_OFFSET)) < 0) {
		if (r == -EIO)
			r = -EINVAL;	/* too short to hold a super block */
		goto fail;
	}
	if ((r = recognise(fs, sb)) < 0)
		goto fail;
	read_super(fs, sb);
	if ((r = check_super(fs)) < 0)
		goto fail;
	if (fs->version == 3 && (fs->state & MFS_FLAG_MANDATORY) != 0) {
		r = -ENOTSUP;
		goto fail;
	}
	fs->ibuf = malloc(fs->block_size);
	fs->dbuf = malloc(fs->block_size);
	if (fs->ibuf == NULL || fs->dbuf == NULL) {
		r = -ENOMEM;
		goto fail;
	}
	return 0;

fail:
	mfs_close(fs);
	return r;
}

int
mfs_open(struct mfs *fs, const char *path)
{
	return mfs_open_tracks(fs, path, 0, NULL);
}

int
mfs_open_rw(struct mfs *fs, const char *path)
{
	return mfs_open_tracks(fs, path, 1, NULL);
}

void
mfs_close(struct mfs *fs)
{
	if (fs->fd != -1)
		(void)close(fs->fd);
	free(fs->ibuf);
	free(fs->dbuf);
	free(fs->imap);
	free(fs->zmap);
	fs->fd = -1;
	fs->ibuf = NULL;
	fs->dbuf = NULL;
	fs->imap = NULL;
	fs->zmap = NULL;
}

int
mfs_read_block(struct mfs *fs, uint32_t block, void *buf)
{
	if (block >= fs->nblocks)
		return -EIO;
	return dev_read(fs, buf, fs->block_size,
	    (off_t)block * fs->block_size);
}

/* Decode a V1 inode at p. */
static void
decode_v1(const struct mfs *fs, const unsigned char *p, struct mfs_inode *ip)
{
	int i;

	ip->mode = get16(fs, p + I1_MODE);
	ip->uid = get16(fs, p + I1_UID);
	ip->size = get32(fs, p + I1_FSIZE);
	ip->mtime = get32(fs, p + I1_MTIME);
	ip->gid = p[I1_GID];
	ip->nlinks = p[I1_NLINKS];
	for (i = 0; i < I1_NZONES; i++)
		ip->zone[i] = get16(fs, p + I1_ZONE + 2 * i);
	ip->atime = ip->mtime;
	ip->ctime = ip->mtime;
}

/* Decode a V2 or V3 inode at p. */
static void
decode_v2(const struct mfs *fs, const unsigned char *p, struct mfs_inode *ip)
{
	int i;

	ip->mode = get16(fs, p + I2_MODE);
	ip->nlinks = get16(fs, p + I2_NLINKS);
	ip->uid = get16(fs, p + I2_UID);
	ip->gid = get16(fs, p + I2_GID);
	ip->size = get32(fs, p + I2_FSIZE);
	ip->atime = get32(fs, p + I2_ATIME);
	ip->mtime = get32(fs, p + I2_MTIME);
	ip->ctime = get32(fs, p + I2_CTIME);
	for (i = 0; i < I2_NZONES; i++)
		ip->zone[i] = get32(fs, p + I2_ZONE + 4 * i);
}

int
mfs_get_inode(struct mfs *fs, uint32_t num, struct mfs_inode *ip)
{
	uint64_t off;
	int r;

	if (num == 0 || num > fs->ninodes)
		return -EIO;
	off = (uint64_t)(num - 1) * fs->inode_size;
	/* The block number fits: check_super() bounded the inode table. */
	r = mfs_read_block(fs, fs->inode_start +
	    (uint32_t)(off / fs->block_size), fs->ibuf);
	if (r < 0)
		return r;

	(void)memset(ip, 0, sizeof(*ip));
	ip->num = num;
	if (fs->version == 1)
		decode_v1(fs, fs->ibuf + off % fs->block_size, ip);
	else
		decode_v2(fs, fs->ibuf + off % fs->block_size, ip);
	return 0;
}

int
mfs_read_inode(struct mfs *fs, uint32_t num, struct mfs_inode *ip)
{
	int r;

	if ((r = mfs_get_inode(fs, num, ip)) < 0)
		return r;
	/* A size the zones cannot reach means a damaged inode. */
	if (ip->size > fs->max_file)
		return -EIO;
	return 0;
}

/* Check a zone number found in an inode or an indirect block. */
static int
check_zone(const struct mfs *fs, uint32_t zone)
{
	if (zone != 0 && (zone < fs->firstdatazone || zone >= fs->nzones))
		return -EIO;
	return 0;
}

/* Fetch entry idx of the indirect block that starts zone ind. */
static int
indirect(struct mfs *fs, uint32_t ind, uint32_t idx, uint32_t *zone)
{
	int r;

	*zone = 0;
	if (ind == 0)
		return 0;
	if ((r = check_zone(fs, ind)) < 0)
		return r;
	if ((r = mfs_read_block(fs, ind << fs->log_zone_size, fs->ibuf)) < 0)
		return r;
	*zone = getn(fs, fs->ibuf + idx * fs->zone_num_size,
	    fs->zone_num_size);
	return check_zone(fs, *zone);
}

int
mfs_bmap(struct mfs *fs, const struct mfs_inode *ip, uint32_t fblock,
    uint32_t *block)
{
	uint64_t idx, per;
	uint32_t level, scale, zone;
	int r;

	*block = 0;
	scale = fs->log_zone_size;
	idx = fblock >> scale;
	if (idx < fs->ndzones) {
		zone = ip->zone[idx];
		if ((r = check_zone(fs, zone)) < 0)
			return r;
	} else {
		/* Find the level of indirection that holds the zone. */
		idx -= fs->ndzones;
		per = 1;
		for (level = 1; level <= fs->nlevels; level++) {
			per *= fs->nindirs;
			if (idx < per)
				break;
			idx -= per;
		}
		if (level > fs->nlevels)
			return -EFBIG;

		/* Walk down from the zone slot of that level. */
		zone = ip->zone[fs->ndzones + level - 1];
		while (level-- > 0) {
			per /= fs->nindirs;
			r = indirect(fs, zone, (uint32_t)(idx / per), &zone);
			if (r < 0)
				return r;
			idx %= per;
		}
	}
	if (zone != 0)
		*block = (zone << scale) + (fblock & ((1U << scale) - 1));
	return 0;
}

ssize_t
mfs_pread(struct mfs *fs, const struct mfs_inode *ip, void *buf,
    size_t len, uint32_t off)
{
	unsigned char *out;
	size_t done, n;
	uint32_t block, boff;
	int r;

	if (mfs_is_dev(ip))
		return -EINVAL;
	if (off >= ip->size)
		return 0;
	if (len > ip->size - off)
		len = ip->size - off;
	if (len > SSIZE_MAX)
		len = SSIZE_MAX;

	out = buf;
	for (done = 0; done < len; done += n, off += (uint32_t)n) {
		boff = off % fs->block_size;
		n = fs->block_size - boff;
		if (n > len - done)
			n = len - done;
		if ((r = mfs_bmap(fs, ip, off / fs->block_size, &block)) < 0)
			return r;
		if (block == 0) {
			(void)memset(out + done, 0, n);
		} else {
			if ((r = mfs_read_block(fs, block, fs->dbuf)) < 0)
				return r;
			(void)memcpy(out + done, fs->dbuf + boff, n);
		}
	}
	return (ssize_t)done;
}

/*
 * The name of the flex entry at p, of which room bytes are left in the
 * block, into name; returns the bytes the entry takes.  A count of extra
 * slots that runs past the block or the longest name is cut down to it.
 */
static uint32_t
flex_name(const unsigned char *p, uint32_t room, char *name)
{
	uint32_t ext, len;

	ext = p[FLEX_EXTENT];
	if (ext > FLEX_MAX_EXTENT)
		ext = FLEX_MAX_EXTENT;
	if ((ext + 1) * FLEX_SLOT > room)
		ext = room / FLEX_SLOT - 1;
	len = (ext + 1) * FLEX_SLOT - FLEX_NAME;
	(void)memcpy(name, p + FLEX_NAME, len);
	name[len - 1] = '\0';
	return (ext + 1) * FLEX_SLOT;
}

int
mfs_readdir(struct mfs *fs, const struct mfs_inode *dp, mfs_dirent_fn fn,
    void *arg)
{
	struct mfs_dirent de;
	unsigned char *buf, *p;
	ssize_t n;
	uint32_t i, len, off;
	int r;

	if (!mfs_is_dir(dp))
		return -ENOTDIR;
	/* fn may call back into the library, so keep a buffer of our own. */
	if ((buf = malloc(fs->block_size)) == NULL)
		return -ENOMEM;
	r = 0;
	for (off = 0; off < dp->size && r == 0; off += fs->block_size) {
		if ((n = mfs_pread(fs, dp, buf, fs->block_size, off)) < 0) {
			r = (int)n;
			break;
		}
		for (i = 0; i + fs->dirent_size <= (size_t)n; i += len) {
			p = buf + i;
			len = fs->dirent_size;
			if (fs->flex)
				len = flex_name(p, (uint32_t)n - i, de.name);
			de.ino = getn(fs, p, fs->dirent_ino);
			if (de.ino == 0)
				continue;
			de.off = off + i;
			if (!fs->flex) {
				(void)memcpy(de.name, p + fs->dirent_ino,
				    fs->namelen);
				de.name[fs->namelen] = '\0';
			}
			if ((r = fn(&de, arg)) != 0)
				break;
		}
	}
	free(buf);
	return r;
}

struct lookup {
	const char	*name;
	uint32_t	ino;
};

static int
lookup_fn(const struct mfs_dirent *de, void *arg)
{
	struct lookup *lk;

	lk = arg;
	if (strcmp(de->name, lk->name) != 0)
		return 0;
	lk->ino = de->ino;
	return 1;
}

int
mfs_lookup(struct mfs *fs, const struct mfs_inode *dp, const char *name,
    uint32_t *ino)
{
	struct lookup lk;
	int r;

	if (strlen(name) > fs->namelen)
		return -ENAMETOOLONG;
	lk.name = name;
	lk.ino = 0;
	if ((r = mfs_readdir(fs, dp, lookup_fn, &lk)) < 0)
		return r;
	if (r == 0)
		return -ENOENT;
	*ino = lk.ino;
	return 0;
}

int
mfs_namei(struct mfs *fs, const char *path, struct mfs_inode *ip)
{
	char name[MFS_MAX_NAME + 1];
	const char *end, *p;
	size_t len;
	uint32_t ino;
	int r;

	if ((r = mfs_read_inode(fs, MFS_ROOT_INO, ip)) < 0)
		return r;
	for (p = path;; p += len) {
		while (*p == '/')
			p++;
		if (*p == '\0')
			return 0;
		if ((end = strchr(p, '/')) != NULL)
			len = (size_t)(end - p);
		else
			len = strlen(p);
		if (len > fs->namelen)
			return -ENAMETOOLONG;
		(void)memcpy(name, p, len);
		name[len] = '\0';
		if (!mfs_is_dir(ip))
			return -ENOTDIR;
		if ((r = mfs_lookup(fs, ip, name, &ino)) < 0)
			return r;
		if ((r = mfs_read_inode(fs, ino, ip)) < 0)
			return r;
	}
}

int
mfs_is_dir(const struct mfs_inode *ip)
{
	return (ip->mode & MFS_S_IFMT) == MFS_S_IFDIR;
}

int
mfs_is_reg(const struct mfs_inode *ip)
{
	return (ip->mode & MFS_S_IFMT) == MFS_S_IFREG;
}

int
mfs_is_lnk(const struct mfs_inode *ip)
{
	return (ip->mode & MFS_S_IFMT) == MFS_S_IFLNK;
}

int
mfs_is_dev(const struct mfs_inode *ip)
{
	return (ip->mode & MFS_S_IFMT) == MFS_S_IFCHR ||
	    (ip->mode & MFS_S_IFMT) == MFS_S_IFBLK;
}

uint32_t
mfs_rdev(const struct mfs_inode *ip)
{
	return ip->zone[0];
}

/*
 * Walk the indirect block at zone ind, of the given level: report it,
 * then what it lists.  A zone outside the data area is reported but not
 * read.
 */
static int
walk_indirect(struct mfs *fs, uint32_t ind, int level,
    const struct mfs_zref *where, mfs_zone_fn fn, void *arg)
{
	struct mfs_zref ref;
	unsigned char *buf;
	uint32_t i, zone;
	int r;

	if ((r = fn(ind, level, where, arg)) == MFS_WALK_SKIP)
		return 0;
	if (r != 0)
		return r;
	if (check_zone(fs, ind) < 0)
		return 0;
	if ((buf = malloc(fs->block_size)) == NULL)
		return -ENOMEM;
	if ((r = mfs_read_block(fs, ind << fs->log_zone_size, buf)) < 0) {
		free(buf);
		return r;
	}
	ref.block = ind << fs->log_zone_size;
	for (i = 0; i < fs->nindirs && r == 0; i++) {
		zone = getn(fs, buf + i * fs->zone_num_size,
		    fs->zone_num_size);
		if (zone == 0)
			continue;
		ref.index = i;
		if (level == 1)
			r = fn(zone, 0, &ref, arg);
		else
			r = walk_indirect(fs, zone, level - 1, &ref, fn, arg);
		/* A data zone lists nothing to skip: go on to the next. */
		if (r == MFS_WALK_SKIP)
			r = 0;
	}
	free(buf);
	return r;
}

int
mfs_walk_zones(struct mfs *fs, const struct mfs_inode *ip, mfs_zone_fn fn,
    void *arg)
{
	struct mfs_zref ref;
	uint32_t i, level;
	int r;

	ref.block = 0;
	for (i = 0; i < fs->ndzones; i++) {
		ref.index = i;
		if (ip->zone[i] != 0 &&
		    (r = fn(ip->zone[i], 0, &ref, arg)) != 0 &&
		    r != MFS_WALK_SKIP)
			return r;
	}
	for (level = 1; level <= fs->nlevels; level++) {
		i = fs->ndzones + level - 1;
		if (ip->zone[i] == 0)
			continue;
		ref.index = i;
		r = walk_indirect(fs, ip->zone[i], (int)level, &ref, fn, arg);
		if (r != 0)
			return r;
	}
	return 0;
}

int
mfs_load_map(struct mfs *fs, enum mfs_map which, unsigned char **mapp)
{
	unsigned char *map;
	uint32_t i, n, start;
	int r;

	if (which == MFS_IMAP) {
		start = START_BLOCK;
		n = fs->imap_blocks;
	} else {
		start = START_BLOCK + fs->imap_blocks;
		n = fs->zmap_blocks;
	}
	if ((map = malloc((size_t)n * fs->block_size)) == NULL)
		return -ENOMEM;
	for (i = 0; i < n; i++) {
		r = mfs_read_block(fs, start + i,
		    map + (size_t)i * fs->block_size);
		if (r < 0) {
			free(map);
			return r;
		}
	}
	*mapp = map;
	return 0;
}

/*
 * The maps are arrays of words, 16 bits wide in V1 and V2 and 32 bits in
 * V3, in the byte order of the image.
 */
int
mfs_map_bit(const struct mfs *fs, const unsigned char *map, uint32_t n)
{
	uint32_t byte;

	byte = n / 8;
	if (fs->order == MFS_BIG_ENDIAN)
		byte ^= fs->version == 3 ? 3 : 1;
	return (map[byte] >> (n % 8)) & 1;
}

/* Count the clear bits among bits 1 to nbits of a map. */
static int
count_clear(struct mfs *fs, enum mfs_map which, uint32_t nbits,
    uint32_t *count)
{
	unsigned char *map;
	uint32_t bit;
	int r;

	if ((r = mfs_load_map(fs, which, &map)) < 0)
		return r;
	*count = 0;
	for (bit = 1; bit <= nbits; bit++)
		if (!mfs_map_bit(fs, map, bit))
			(*count)++;
	free(map);
	return 0;
}

int
mfs_count_free(struct mfs *fs, uint32_t *inodes, uint32_t *zones)
{
	int r;

	if ((r = count_clear(fs, MFS_IMAP, fs->ninodes, inodes)) < 0)
		return r;
	return count_clear(fs, MFS_ZMAP, fs->nzones - fs->firstdatazone,
	    zones);
}

int
mfs_is_clean(const struct mfs *fs)
{
	if (fs->vmd)
		return (fs->state & MFS_VMD_CLEAN) != 0;
	if (fs->version == 3)
		return (fs->state & MFS_FLAG_CLEAN) != 0;
	return (fs->state & (MFS_STATE_VALID | MFS_STATE_ERROR)) ==
	    MFS_STATE_VALID;
}

int
mfs_put_super(struct mfs *fs)
{
	unsigned char sb[SUPER_SIZE];
	int r;

	if (!fs->writable)
		return -EROFS;
	if ((r = dev_read(fs, sb, sizeof(sb), SUPER_OFFSET)) < 0)
		return r;
	if (fs->version == 3) {
		put32(fs->order, sb + SB3_MAXSIZE, fs->max_size);
		put16(fs->order, sb + SB3_FLAGS, fs->state);
	} else if (fs->vmd) {
		put32(fs->order, sb + SB12_MAXSIZE, fs->max_size);
		sb[SBVMD_FLAGS] = (unsigned char)fs->state;
	} else {
		put32(fs->order, sb + SB12_MAXSIZE, fs->max_size);
		put16(fs->order, sb + SB12_STATE, fs->state);
	}
	return dev_write(fs, sb, sizeof(sb), SUPER_OFFSET);
}

int
mfs_mark_clean(struct mfs *fs, int clean)
{
	if (fs->vmd) {
		/* Minix-vmd has no mark for errors: not clean says it. */
		if (clean)
			fs->state |= MFS_VMD_CLEAN;
		else
			fs->state &= (uint16_t)~MFS_VMD_CLEAN;
	} else if (fs->version == 3) {
		if (clean)
			fs->state |= MFS_FLAG_CLEAN;
		else
			fs->state &= (uint16_t)~MFS_FLAG_CLEAN;
	} else {
		fs->state |= MFS_STATE_VALID;
		if (clean)
			fs->state &= (uint16_t)~MFS_STATE_ERROR;
		else
			fs->state |= MFS_STATE_ERROR;
	}
	return mfs_put_super(fs);
}

int
mfs_write_block(struct mfs *fs, uint32_t block, const void *buf)
{
	if (!fs->writable)
		return -EROFS;
	if (block >= fs->nblocks)
		return -EIO;
	return dev_write(fs, buf, fs->block_size,
	    (off_t)block * fs->block_size);
}

/* Encode *ip as a V1 inode at p. */
static void
encode_v1(const struct mfs *fs, const struct mfs_inode *ip, unsigned char *p)
{
	int i;

	put16(fs->order, p + I1_MODE, ip->mode);
	put16(fs->order, p + I1_UID, ip->uid);
	put32(fs->order, p + I1_FSIZE, ip->size);
	put32(fs->order, p + I1_MTIME, ip->mtime);
	p[I1_GID] = (unsigned char)ip->gid;
	p[I1_NLINKS] = (unsigned char)ip->nlinks;
	for (i = 0; i < I1_NZONES; i++)
		put16(fs->order, p + I1_ZONE + 2 * i, ip->zone[i]);
}

/* Encode *ip as a V2 or V3 inode at p. */
static void
encode_v2(const struct mfs *fs, const struct mfs_inode *ip, unsigned char *p)
{
	int i;

	put16(fs->order, p + I2_MODE, ip->mode);
	put16(fs->order, p + I2_NLINKS, ip->nlinks);
	put16(fs->order, p + I2_UID, ip->uid);
	put16(fs->order, p + I2_GID, ip->gid);
	put32(fs->order, p + I2_FSIZE, ip->size);
	put32(fs->order, p + I2_ATIME, ip->atime);
	put32(fs->order, p + I2_MTIME, ip->mtime);
	put32(fs->order, p + I2_CTIME, ip->ctime);
	for (i = 0; i < I2_NZONES; i++)
		put32(fs->order, p + I2_ZONE + 4 * i, ip->zone[i]);
}

int
mfs_put_inode(struct mfs *fs, const struct mfs_inode *ip)
{
	uint64_t off;
	uint32_t block;
	int r;

	if (ip->num == 0 || ip->num > fs->ninodes)
		return -EIO;
	off = (uint64_t)(ip->num - 1) * fs->inode_size;
	block = fs->inode_start + (uint32_t)(off / fs->block_size);
	if ((r = mfs_read_block(fs, block, fs->ibuf)) < 0)
		return r;
	if (fs->version == 1)
		encode_v1(fs, ip, fs->ibuf + off % fs->block_size);
	else
		encode_v2(fs, ip, fs->ibuf + off % fs->block_size);
	return mfs_write_block(fs, block, fs->ibuf);
}

/*
 * Read the block that holds byte off of the directory *dp into fs->dbuf
 * and store its number in *block.
 */
static int
entry_block(struct mfs *fs, const struct mfs_inode *dp, uint32_t off,
    uint32_t *block)
{
	int r;

	if (off % fs->dirent_size != 0)
		return -EINVAL;
	if ((r = mfs_bmap(fs, dp, off / fs->block_size, block)) < 0)
		return r;
	if (*block == 0)
		return -EIO;
	return mfs_read_block(fs, *block, fs->dbuf);
}

static void
put_ino(const struct mfs *fs, unsigned char *p, uint32_t ino)
{
	if (fs->dirent_ino == 2)
		put16(fs->order, p, ino);
	else
		put32(fs->order, p, ino);
}

int
mfs_set_entry(struct mfs *fs, const struct mfs_inode *dp, uint32_t off,
    uint32_t ino)
{
	uint32_t block;
	int r;

	if (off >= dp->size)
		return -EINVAL;
	if ((r = entry_block(fs, dp, off, &block)) < 0)
		return r;
	put_ino(fs, fs->dbuf + off % fs->block_size, ino);
	return mfs_write_block(fs, block, fs->dbuf);
}

int
mfs_put_entry(struct mfs *fs, const struct mfs_inode *dp, uint32_t off,
    uint32_t ino, const char *name)
{
	unsigned char *p;
	uint32_t block;
	size_t len;
	int r;

	/* Entries of flex directories are not all the same size. */
	if (fs->flex)
		return -ENOTSUP;
	if ((len = strlen(name)) > fs->namelen)
		return -ENAMETOOLONG;
	if ((r = entry_block(fs, dp, off, &block)) < 0)
		return r;
	p = fs->dbuf + off % fs->block_size;
	put_ino(fs, p, ino);
	(void)memset(p + fs->dirent_ino, 0, fs->namelen);
	(void)memcpy(p + fs->dirent_ino, name, len);
	return mfs_write_block(fs, block, fs->dbuf);
}

int
mfs_clear_zref(struct mfs *fs, struct mfs_inode *ip,
    const struct mfs_zref *ref)
{
	return mfs_set_zref(fs, ip, ref, 0);
}

int
mfs_set_zref(struct mfs *fs, struct mfs_inode *ip,
    const struct mfs_zref *ref, uint32_t zone)
{
	unsigned char *p;
	int r;

	if (ref->block == 0) {
		if (ref->index >= MFS_NR_ZONES)
			return -EINVAL;
		ip->zone[ref->index] = zone;
		return 0;
	}
	if (ref->index >= fs->nindirs)
		return -EINVAL;
	if ((r = mfs_read_block(fs, ref->block, fs->dbuf)) < 0)
		return r;
	p = fs->dbuf + ref->index * fs->zone_num_size;
	if (fs->zone_num_size == 2)
		put16(fs->order, p, zone);
	else
		put32(fs->order, p, zone);
	return mfs_write_block(fs, ref->block, fs->dbuf);
}

void
mfs_set_map_bit(const struct mfs *fs, unsigned char *map, uint32_t n, int v)
{
	uint32_t byte;

	byte = n / 8;
	if (fs->order == MFS_BIG_ENDIAN)
		byte ^= fs->version == 3 ? 3 : 1;
	if (v)
		map[byte] |= (unsigned char)(1 << (n % 8));
	else
		map[byte] &= (unsigned char)~(1 << (n % 8));
}

int
mfs_store_map(struct mfs *fs, enum mfs_map which, const unsigned char *map)
{
	uint32_t i, n, start;
	int r;

	if (which == MFS_IMAP) {
		start = START_BLOCK;
		n = fs->imap_blocks;
	} else {
		start = START_BLOCK + fs->imap_blocks;
		n = fs->zmap_blocks;
	}
	for (i = 0; i < n; i++) {
		r = mfs_write_block(fs, start + i,
		    map + (size_t)i * fs->block_size);
		if (r < 0)
			return r;
	}
	return 0;
}
