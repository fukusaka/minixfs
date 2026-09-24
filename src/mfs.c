/*
 * mfs.c - read access to MINIX file system images.
 *
 * On-disk layout of a V1 file system (1024-byte blocks):
 *
 *	block 0			boot block
 *	block 1			super block
 *	block 2 ...		inode bit map (imap_blocks)
 *	...			zone bit map (zmap_blocks)
 *	...			inode table (32-byte inodes)
 *	firstdatazone ...	data zones
 *
 * A zone is 2^log_zone_size blocks.  The zone slots of an inode are 7
 * direct zones, then a single and a double indirect zone; zone numbers
 * are 16 bits wide.  A directory entry is a 16-bit inode number and a
 * name of 14 characters (30 in the Linux extension).
 *
 * Everything is stored in the byte order of the machine that made the
 * file system: little-endian on the PC, big-endian on the 68000 (Atari
 * ST, Amiga, Macintosh).  The byte order is found from the magic number.
 *
 * The layout follows fs/super.h, fs/inode.h and fs/type.h of MINIX 2.0.4.
 */

#include <sys/stat.h>

#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "mfs.h"

#define SUPER_OFFSET	1024		/* byte offset of the super block */
#define SUPER_SIZE	1024		/* bytes read for the super block */
#define START_BLOCK	2		/* first block of the inode map */
#define BLOCK_SIZE	1024
#define NR_DZONES	7		/* direct zones in an inode */
#define NR_LEVELS	2		/* levels of indirection */
#define ZONE_NUM_SIZE	2		/* bytes of a zone number */
#define DIRENT_INO	2		/* bytes of a directory inode number */
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

/* The V3 magic number is further on. */
#define SB3_MAGIC	24		/* 16 */

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

/* Is the 16-bit number at p magic, in either byte order? */
static int
is_magic(const unsigned char *p, uint16_t magic)
{
	return (p[1] << 8 | p[0]) == magic || (p[0] << 8 | p[1]) == magic;
}

/*
 * Recognise the magic number and set the version and byte order.
 * Returns 0 for V1, -ENOTSUP for V2 and V3, which cannot be read yet, or
 * -EINVAL if this is not a MINIX file system.
 */
static int
recognise(struct mfs *fs, const unsigned char *sb)
{
	static const uint16_t v1[] = { MFS_MAGIC_V1, MFS_MAGIC_V1L };
	const unsigned char *p;
	size_t i;

	p = sb + SB12_MAGIC;
	for (i = 0; i < sizeof(v1) / sizeof(v1[0]); i++) {
		if ((p[1] << 8 | p[0]) == v1[i])
			fs->order = MFS_LITTLE_ENDIAN;
		else if ((p[0] << 8 | p[1]) == v1[i])
			fs->order = MFS_BIG_ENDIAN;
		else
			continue;
		fs->magic = v1[i];
		fs->version = 1;
		return 0;
	}
	if (is_magic(p, MFS_MAGIC_V2) || is_magic(p, MFS_MAGIC_V2L) ||
	    is_magic(sb + SB3_MAGIC, MFS_MAGIC_V3))
		return -ENOTSUP;
	return -EINVAL;
}

/* Fill in the fields of the super block and those that follow from them. */
static void
read_super(struct mfs *fs, const unsigned char *sb)
{
	fs->ninodes = get16(fs, sb + SB12_NINODES);
	fs->nzones = get16(fs, sb + SB12_NZONES);
	fs->imap_blocks = get16(fs, sb + SB12_IMAP);
	fs->zmap_blocks = get16(fs, sb + SB12_ZMAP);
	fs->firstdatazone = get16(fs, sb + SB12_FIRSTDATA);
	fs->log_zone_size = get16(fs, sb + SB12_LOGZONE);
	fs->max_size = get32(fs, sb + SB12_MAXSIZE);
	fs->block_size = BLOCK_SIZE;

	fs->namelen = fs->magic == MFS_MAGIC_V1 ? 14 : 30;
	fs->dirent_size = DIRENT_INO + fs->namelen;
	fs->inode_size = I1_SIZE;
	fs->ndzones = NR_DZONES;
	fs->nindirs = BLOCK_SIZE / ZONE_NUM_SIZE;
	fs->inode_start = START_BLOCK + fs->imap_blocks + fs->zmap_blocks;
}

/* Bytes a file can have: the zones its slots reach, times the zone size. */
static uint64_t
max_file_size(const struct mfs *fs)
{
	uint64_t zones;

	zones = fs->ndzones + fs->nindirs + (uint64_t)fs->nindirs * fs->nindirs;
	return zones * ((uint64_t)fs->block_size << fs->log_zone_size);
}

/* Check that the super block describes a layout that makes sense. */
static int
check_super(struct mfs *fs)
{
	uint64_t data, itable, nblocks;

	if (fs->ninodes == 0 || fs->nzones == 0 || fs->imap_blocks == 0 ||
	    fs->zmap_blocks == 0 || fs->log_zone_size > MAX_LOG_ZONE)
		return -EINVAL;
	if (fs->firstdatazone >= fs->nzones)
		return -EINVAL;

	/* The bit maps must have a bit for every inode and zone. */
	if ((uint64_t)fs->imap_blocks * fs->block_size * 8 <
	    (uint64_t)fs->ninodes + 1)
		return -EINVAL;
	if ((uint64_t)fs->zmap_blocks * fs->block_size * 8 <
	    (uint64_t)fs->nzones - fs->firstdatazone + 1)
		return -EINVAL;

	itable = ((uint64_t)fs->ninodes * fs->inode_size + fs->block_size - 1)
	    / fs->block_size;
	data = (uint64_t)fs->firstdatazone << fs->log_zone_size;
	nblocks = (uint64_t)fs->nzones << fs->log_zone_size;
	if (fs->inode_start + itable > data || data > nblocks ||
	    nblocks > UINT32_MAX)
		return -EINVAL;
	fs->nblocks = (uint32_t)nblocks;
	fs->max_file = max_file_size(fs);
	return 0;
}

int
mfs_open(struct mfs *fs, const char *path)
{
	unsigned char sb[SUPER_SIZE];
	struct stat st;
	int r;

	(void)memset(fs, 0, sizeof(*fs));
	if ((fs->fd = open(path, O_RDONLY)) == -1)
		return -errno;
	if (fstat(fs->fd, &st) == -1) {
		r = -errno;
		goto fail;
	}
	fs->image_size = st.st_size;
	if ((r = read_at(fs->fd, sb, sizeof(sb), SUPER_OFFSET)) < 0) {
		if (r == -EIO)
			r = -EINVAL;	/* too short to hold a super block */
		goto fail;
	}
	if ((r = recognise(fs, sb)) < 0)
		goto fail;
	read_super(fs, sb);
	if ((r = check_super(fs)) < 0)
		goto fail;
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

void
mfs_close(struct mfs *fs)
{
	if (fs->fd != -1)
		(void)close(fs->fd);
	free(fs->ibuf);
	free(fs->dbuf);
	fs->fd = -1;
	fs->ibuf = NULL;
	fs->dbuf = NULL;
}

int
mfs_read_block(struct mfs *fs, uint32_t block, void *buf)
{
	if (block >= fs->nblocks)
		return -EIO;
	return read_at(fs->fd, buf, fs->block_size,
	    (off_t)block * fs->block_size);
}

int
mfs_read_inode(struct mfs *fs, uint32_t num, struct mfs_inode *ip)
{
	const unsigned char *p;
	uint64_t off;
	int i, r;

	if (num == 0 || num > fs->ninodes)
		return -EIO;
	off = (uint64_t)(num - 1) * fs->inode_size;
	/* The block number fits: check_super() bounded the inode table. */
	r = mfs_read_block(fs, fs->inode_start +
	    (uint32_t)(off / fs->block_size), fs->ibuf);
	if (r < 0)
		return r;
	p = fs->ibuf + off % fs->block_size;

	(void)memset(ip, 0, sizeof(*ip));
	ip->num = num;
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
	*zone = get16(fs, fs->ibuf + idx * ZONE_NUM_SIZE);
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
		for (level = 1; level <= NR_LEVELS; level++) {
			per *= fs->nindirs;
			if (idx < per)
				break;
			idx -= per;
		}
		if (level > NR_LEVELS)
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

int
mfs_readdir(struct mfs *fs, const struct mfs_inode *dp, mfs_dirent_fn fn,
    void *arg)
{
	struct mfs_dirent de;
	unsigned char *buf, *p;
	ssize_t n;
	uint32_t i, off;
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
		for (i = 0; i + fs->dirent_size <= (size_t)n;
		    i += fs->dirent_size) {
			p = buf + i;
			de.ino = get16(fs, p);
			if (de.ino == 0)
				continue;
			(void)memcpy(de.name, p + DIRENT_INO, fs->namelen);
			de.name[fs->namelen] = '\0';
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
